
#include <winstd.H>

#include <Profiler.H>
#include <BoxArray.H>
#include <DistributionMapping.H>
#include <ParallelDescriptor.H>
#include <ParmParse.H>
#include <Profiler.H>
#include <FArrayBox.H>

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <list>
#include <map>
#include <vector>
#include <queue>
#include <algorithm>
#include <numeric>
#include <string>
#include <cstring>
using std::string;

namespace
{
    bool initialized = false;
    std::map<int, int> rankPNumMap;       // [rank, procNumber]
    std::multimap<int, int> pNumRankMM;   // [procNumber, rank]
    std::map<int, IntVect> pNumTopIVMap;  // [procNumber, topological iv position]
    std::multimap<IntVect, int, IntVect::Compare> topIVpNumMM;
                                          // [topological iv position, procNumber]
    std::vector<int> ranksSFC;
}

namespace
{
    //
    // Set default values for these in Initialize()!!!
    //
    bool   verbose;
    int    sfc_threshold;
    double max_efficiency;
}

// We default to SFC.
DistributionMapping::Strategy DistributionMapping::m_Strategy = DistributionMapping::SFC;

DistributionMapping::PVMF DistributionMapping::m_BuildMap = 0;

long DistributionMapping::totalCells(0);
Real DistributionMapping::bytesPerCell(0.0);
Array<int> DistributionMapping::proximityMap;
Array<int> DistributionMapping::proximityOrder;
Array<long> DistributionMapping::totalBoxPoints;

const Array<int>&
DistributionMapping::ProcessorMap () const
{
    return m_ref->m_pmap;
}

DistributionMapping::Strategy
DistributionMapping::strategy ()
{
    return DistributionMapping::m_Strategy;
}

int
DistributionMapping::CacheSize ()
{
    return m_Cache.size();
}

void
DistributionMapping::strategy (DistributionMapping::Strategy how)
{
    DistributionMapping::m_Strategy = how;

    switch (how)
    {
    case ROUNDROBIN:
        m_BuildMap = &DistributionMapping::RoundRobinProcessorMap;
        break;
    case KNAPSACK:
        m_BuildMap = &DistributionMapping::KnapSackProcessorMap;
        break;
    case SFC:
        m_BuildMap = &DistributionMapping::SFCProcessorMap;
        break;
    case PFC:
        m_BuildMap = &DistributionMapping::PFCProcessorMap;
        break;
    default:
        BoxLib::Error("Bad DistributionMapping::Strategy");
    }
}

void
DistributionMapping::SFC_Threshold (int n)
{
    sfc_threshold = std::min(n,1);
}

int
DistributionMapping::SFC_Threshold ()
{
    return sfc_threshold;
}

bool
DistributionMapping::operator== (const DistributionMapping& rhs) const
{
    return m_ref == rhs.m_ref || m_ref->m_pmap == rhs.m_ref->m_pmap;
}

bool
DistributionMapping::operator!= (const DistributionMapping& rhs) const
{
    return !operator==(rhs);
}

void
DistributionMapping::Initialize ()
{
    if (initialized) return;
    //
    // Set defaults here!!!
    //
    verbose          = false;
    sfc_threshold    = 0;
    max_efficiency   = 0.9;

    ParmParse pp("DistributionMapping");

    pp.query("v"      ,          verbose);
    pp.query("verbose",          verbose);
    pp.query("efficiency",       max_efficiency);
    pp.query("sfc_threshold",    sfc_threshold);

    std::string theStrategy;

    if (pp.query("strategy", theStrategy))
    {
        if (theStrategy == "ROUNDROBIN")
        {
            strategy(ROUNDROBIN);
        }
        else if (theStrategy == "KNAPSACK")
        {
            strategy(KNAPSACK);
        }
        else if (theStrategy == "SFC")
        {
            strategy(SFC);
        }
        else if (theStrategy == "PFC")
        {
            strategy(PFC);
	    DistributionMapping::InitProximityMap();
        }
        else
        {
            std::string msg("Unknown strategy: ");
            msg += theStrategy;
            BoxLib::Warning(msg.c_str());
        }
    }
    else
    {
        strategy(m_Strategy);  // default
    }

    if(proximityMap.size() != ParallelDescriptor::NProcs()) {
      //std::cout << "#00#::Initialize: proximityMap not resized yet." << std::endl;
      proximityMap.resize(ParallelDescriptor::NProcs(), 0);
      proximityOrder.resize(ParallelDescriptor::NProcs(), 0);
    }
    totalBoxPoints.resize(ParallelDescriptor::NProcs(), 0);

    BoxLib::ExecOnFinalize(DistributionMapping::Finalize);

    initialized = true;
}

void
DistributionMapping::Finalize ()
{
    initialized = false;

    DistributionMapping::FlushCache();

    DistributionMapping::m_BuildMap = 0;

    DistributionMapping::m_Cache.clear();
}

//
// Our cache of processor maps.
//
std::map< int,LnClassPtr<DistributionMapping::Ref> > DistributionMapping::m_Cache;

void
DistributionMapping::Sort (std::vector<LIpair>& vec,
                           bool                 reverse)
{
    if (vec.size() > 1)
    {
        std::stable_sort(vec.begin(), vec.end(), LIpairComp());

        if (reverse)
        {
            std::reverse(vec.begin(), vec.end());
        }
    }
}

void
DistributionMapping::LeastUsedCPUs (int         nprocs,
                                    Array<int>& result)
{
    result.resize(nprocs);

#ifdef BL_USE_MPI
    BL_PROFILE("DistributionMapping::LeastUsedCPUs()");

    Array<long> bytes(nprocs);

    MPI_Allgather(&BoxLib::total_bytes_allocated_in_fabs,
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  bytes.dataPtr(),
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  ParallelDescriptor::Communicator());

    std::vector<LIpair> LIpairV;

    LIpairV.reserve(nprocs);

    for (int i(0); i < nprocs; ++i)
    {
        LIpairV.push_back(LIpair(bytes[i],i));
    }

    bytes.clear();

    Sort(LIpairV, false);

    for (int i(0); i < nprocs; ++i)
    {
        result[i] = LIpairV[i].second;
    }
#else
    for (int i(0); i < nprocs; ++i)
    {
        result[i] = i;
    }
#endif
}

bool
DistributionMapping::GetMap (const BoxArray& boxes)
{
    const int N = boxes.size();

    BL_ASSERT(m_ref->m_pmap.size() == N + 1);

    std::map< int,LnClassPtr<Ref> >::const_iterator it = m_Cache.find(N+1);

    if (it != m_Cache.end())
    {
        m_ref = it->second;

        BL_ASSERT(m_ref->m_pmap[N] == ParallelDescriptor::MyProc());

        return true;
    }

    return false;
}

DistributionMapping::Ref::Ref () {}

DistributionMapping::DistributionMapping ()
    :
    m_ref(new DistributionMapping::Ref)
{}

DistributionMapping::DistributionMapping (const DistributionMapping& rhs)
    :
    m_ref(rhs.m_ref)
{}

DistributionMapping&
DistributionMapping::operator= (const DistributionMapping& rhs)
{
    m_ref = rhs.m_ref;

    return *this;
}

DistributionMapping::Ref::Ref (const Array<int>& pmap)
    :
    m_pmap(pmap)
{}

DistributionMapping::DistributionMapping (const Array<int>& pmap, bool put_in_cache)
    :
    m_ref(new DistributionMapping::Ref(pmap))
{
    if (put_in_cache && ParallelDescriptor::NProcs() > 1)
    {
        //
        // We want to save this pmap in the cache.
        // It's an error if a pmap of this length has already been cached.
        //
        for (std::map< int,LnClassPtr<Ref> >::const_iterator it = m_Cache.begin();
             it != m_Cache.end();
             ++it)
        {
            if (it->first == m_ref->m_pmap.size())
            {
                BoxLib::Abort("DistributionMapping::DistributionMapping: pmap of given length already exists");
            }
        }

        m_Cache.insert(std::make_pair(m_ref->m_pmap.size(),m_ref));
    }
}

DistributionMapping::Ref::Ref (int len)
    :
    m_pmap(len)
{}

DistributionMapping::DistributionMapping (const BoxArray& boxes, int nprocs)
    :
    m_ref(new DistributionMapping::Ref(boxes.size() + 1))
{
    define(boxes,nprocs);
}

DistributionMapping::Ref::Ref (const Ref& rhs)
    :
    m_pmap(rhs.m_pmap)
{}

DistributionMapping::DistributionMapping (const DistributionMapping& d1,
                                          const DistributionMapping& d2)
    :
    m_ref(new DistributionMapping::Ref(d1.size() + d2.size() - 1))

{
    const Array<int>& pmap_1 = d1.ProcessorMap();
    const Array<int>& pmap_2 = d2.ProcessorMap();

    const int L1 = pmap_1.size() - 1; // Length not including sentinel.
    const int L2 = pmap_2.size() - 1; // Length not including sentinel.

    for (int i = 0; i < L1; i++)
        m_ref->m_pmap[i] = pmap_1[i];

    for (int i = L1, j = 0; j < L2; i++, j++)
        m_ref->m_pmap[i] = pmap_2[j];
    //
    // Set sentinel equal to our processor number.
    //
    m_ref->m_pmap[m_ref->m_pmap.size()-1] = ParallelDescriptor::MyProc();
}

void
DistributionMapping::define (const BoxArray& boxes, int nprocs)
{
    Initialize();

    if (m_ref->m_pmap.size() != boxes.size() + 1)
    {
        m_ref->m_pmap.resize(boxes.size() + 1);

        //m_ref->boxPoints.resize(ParallelDescriptor::NProcs(), 0);
    }

    if (nprocs == 1)
    {
        for (int i = 0, N = m_ref->m_pmap.size(); i < N; i++)
        {
            m_ref->m_pmap[i] = 0;
        }
    }
    else
    {
        if ( ! GetMap(boxes))
        {
if(ParallelDescriptor::IOProcessor()) {
  //std::cout << "|||| DistributionMapping::define:  nprocs boxes.size() = "
            //<< nprocs << "  " << boxes.size() << std::endl;
}
	    BL_ASSERT(m_BuildMap != 0);

            (this->*m_BuildMap)(boxes,nprocs);
            //
            // Add the new processor map to the cache.
            //
            m_Cache.insert(std::make_pair(m_ref->m_pmap.size(),m_ref));

	    // save the number of points
	    if(nprocs == ParallelDescriptor::NProcs()) {
	      //for(int i(0); i < boxes.size(); ++i) {
		//int p(m_ref->m_pmap[i]);
	        //m_ref->boxPoints[p] += boxes[i].numPts();
	      //}
	      //for(int i(0); i < nprocs; ++i) {
	        //totalBoxPoints[i] += m_ref->boxPoints[i];
	      //}
	    } else {
	      // must be making a special distmap
	    }
        }
    }
}

DistributionMapping::~DistributionMapping () { }

void
DistributionMapping::FlushCache ()
{
    CacheStats(std::cout);
    //
    // Remove maps that aren't referenced anywhere else.
    //
    std::map< int,LnClassPtr<Ref> >::iterator it = m_Cache.begin();

    while (it != m_Cache.end())
    {
        if (it->second.linkCount() == 1)
        {
  for(int i(0); i < totalBoxPoints.size(); ++i) {
    //totalBoxPoints[i] -= (it->second)->boxPoints[i];
  }


            m_Cache.erase(it++);
        }
        else
        {
            ++it;
        }
    }

    //totalCells = 0;
    if(ParallelDescriptor::IOProcessor()) {
      std::cout << "_here 1 totalCells = " << totalCells << std::endl;
    }
}

void
DistributionMapping::RoundRobinDoIt (int                  nboxes,
                                     int                  nprocs,
                                     std::vector<LIpair>* LIpairV)
{
//BoxLib::Abort("RoundRobinDoIt");
    Array<int> ord;

    LeastUsedCPUs(nprocs,ord);

    if (LIpairV)
    {
        BL_ASSERT(LIpairV->size() == nboxes);

        for (int i = 0; i < nboxes; i++)
        {
            m_ref->m_pmap[(*LIpairV)[i].second] = ord[i%nprocs];
        }
    }
    else
    {
        for (int i = 0; i < nboxes; i++)
        {
            m_ref->m_pmap[i] = ord[i%nprocs];
        }
    }
    //
    // Set sentinel equal to our processor number.
    //
    m_ref->m_pmap[nboxes] = ParallelDescriptor::MyProc();
}

void
DistributionMapping::RoundRobinProcessorMap (int nboxes, int nprocs)
{
    BL_ASSERT(nboxes > 0);

    if (m_ref->m_pmap.size() != nboxes + 1)
    {
        m_ref->m_pmap.resize(nboxes + 1);
    }

    RoundRobinDoIt(nboxes, nprocs);
}

void
DistributionMapping::RoundRobinProcessorMap (const BoxArray& boxes, int nprocs)
{
    BL_ASSERT(boxes.size() > 0);
    BL_ASSERT(m_ref->m_pmap.size() == boxes.size() + 1);
    //
    // Create ordering of boxes from largest to smallest.
    // When we round-robin the boxes we want to go from largest
    // to smallest box, starting from the CPU having the least
    // amount of FAB data to the one having the most.  This "should"
    // help even out the FAB data distribution when running on large
    // numbers of CPUs, where the lower levels of the calculation are
    // using RoundRobin to lay out fewer than NProc boxes across
    // the CPUs.
    //
    std::vector<LIpair> LIpairV;

    const int N = boxes.size();

    LIpairV.reserve(N);

    for (int i = 0; i < N; i++)
    {
        LIpairV.push_back(LIpair(boxes[i].numPts(),i));
    }

    Sort(LIpairV, true);

    RoundRobinDoIt(boxes.size(), nprocs, &LIpairV);
}

class WeightedBox
{
    int  m_boxid;
    long m_weight;
public:
    WeightedBox () {}
    WeightedBox (int b, int w) : m_boxid(b), m_weight(w) {}
    long weight () const { return m_weight; }
    int  boxid ()  const { return m_boxid;  }

    bool operator< (const WeightedBox& rhs) const
    {
        return weight() > rhs.weight();
    }
};

class WeightedBoxList
{
    std::list<WeightedBox>* m_lb;
    long                    m_weight;
public:
    WeightedBoxList (std::list<WeightedBox>* lb) : m_lb(lb), m_weight(0) {}
    long weight () const
    {
        return m_weight;
    }
    void erase (std::list<WeightedBox>::iterator& it)
    {
        m_weight -= it->weight();
        m_lb->erase(it);
    }
    void push_back (const WeightedBox& bx)
    {
        m_weight += bx.weight();
        m_lb->push_back(bx);
    }
    int size () const { return m_lb->size(); }
    std::list<WeightedBox>::const_iterator begin () const { return m_lb->begin(); }
    std::list<WeightedBox>::iterator begin ()             { return m_lb->begin(); }
    std::list<WeightedBox>::const_iterator end () const   { return m_lb->end();   }
    std::list<WeightedBox>::iterator end ()               { return m_lb->end();   }

    bool operator< (const WeightedBoxList& rhs) const
    {
        return weight() > rhs.weight();
    }
};

static
void
knapsack (const std::vector<long>&         wgts,
          int                              nprocs,
          std::vector< std::vector<int> >& result,
          double&                          efficiency,
          bool                             do_full_knapsack)
{
    //
    // Sort balls by size largest first.
    //
    result.resize(nprocs);

    std::vector<WeightedBox> lb;
    lb.reserve(wgts.size());
    for (unsigned int i = 0, N = wgts.size(); i < N; ++i)
    {
        lb.push_back(WeightedBox(i, wgts[i]));
    }
    BL_ASSERT(lb.size() == wgts.size());
    std::sort(lb.begin(), lb.end());
    BL_ASSERT(lb.size() == wgts.size());
    //
    // For each ball, starting with heaviest, assign ball to the lightest box.
    //
    std::priority_queue<WeightedBoxList>   wblq;
    std::vector< std::list<WeightedBox>* > vbbs(nprocs);
    for (int i  = 0; i < nprocs; ++i)
    {
        vbbs[i] = new std::list<WeightedBox>;
        wblq.push(WeightedBoxList(vbbs[i]));
    }
    BL_ASSERT(int(wblq.size()) == nprocs);
    for (unsigned int i = 0, N = wgts.size(); i < N; ++i)
    {
        WeightedBoxList wbl = wblq.top();
        wblq.pop();
        wbl.push_back(lb[i]);
        wblq.push(wbl);
    }
    BL_ASSERT(int(wblq.size()) == nprocs);
    std::list<WeightedBoxList> wblqg;
    while (!wblq.empty())
    {
        wblqg.push_back(wblq.top());
        wblq.pop();
    }
    BL_ASSERT(int(wblqg.size()) == nprocs);
    wblqg.sort();
    //
    // Compute the max weight and the sum of the weights.
    //
    double max_weight = 0;
    double sum_weight = 0;
    std::list<WeightedBoxList>::iterator it = wblqg.begin();
    for (std::list<WeightedBoxList>::const_iterator End =  wblqg.end(); it != End; ++it)
    {
        long wgt = (*it).weight();
        sum_weight += wgt;
        max_weight = (wgt > max_weight) ? wgt : max_weight;
    }

    efficiency = sum_weight/(nprocs*max_weight);

top:

    std::list<WeightedBoxList>::iterator it_top = wblqg.begin();

    WeightedBoxList wbl_top = *it_top;
    //
    // For each ball in the heaviest box.
    //
    std::list<WeightedBox>::iterator it_wb = wbl_top.begin();

    if (efficiency > max_efficiency || !do_full_knapsack) goto bottom;

    for ( ; it_wb != wbl_top.end(); ++it_wb )
    {
        //
        // For each ball not in the heaviest box.
        //
        std::list<WeightedBoxList>::iterator it_chk = it_top;
        it_chk++;
        for ( ; it_chk != wblqg.end(); ++it_chk)
        {
            WeightedBoxList wbl_chk = *it_chk;
            std::list<WeightedBox>::iterator it_owb = wbl_chk.begin();
            for ( ; it_owb != wbl_chk.end(); ++it_owb)
            {
                //
                // If exchanging these two balls reduces the load balance,
                // then exchange them and go to top.  The way we are doing
                // things, sum_weight cannot change.  So the efficiency will
                // increase if after we switch the two balls *it_wb and
                // *it_owb the max weight is reduced.
                //
                double w_tb = (*it_top).weight() + (*it_owb).weight() - (*it_wb).weight();
                double w_ob = (*it_chk).weight() + (*it_wb).weight() - (*it_owb).weight();
                //
                // If the other ball reduces the weight of the top box when
                // swapped, then it will change the efficiency.
                //
                if (w_tb < (*it_top).weight() && w_ob < (*it_top).weight())
                {
                    //
                    // Adjust the sum weight and the max weight.
                    //
                    WeightedBox wb = *it_wb;
                    WeightedBox owb = *it_owb;
                    wblqg.erase(it_top);
                    wblqg.erase(it_chk);
                    wbl_top.erase(it_wb);
                    wbl_chk.erase(it_owb);
                    wbl_top.push_back(owb);
                    wbl_chk.push_back(wb);
                    std::list<WeightedBoxList> tmp;
                    tmp.push_back(wbl_top);
                    tmp.push_back(wbl_chk);
                    tmp.sort();
                    wblqg.merge(tmp);
                    max_weight = (*wblqg.begin()).weight();
                    efficiency = sum_weight/(nprocs*max_weight);
                    goto top;
                }
            }
        }
    }

 bottom:
    //
    // Here I am "load-balanced".
    //
    std::list<WeightedBoxList>::const_iterator cit = wblqg.begin();

    for (int i = 0; i < nprocs; ++i)
    {
        const WeightedBoxList& wbl = *cit;

        result[i].reserve(wbl.size());

        for (std::list<WeightedBox>::const_iterator it1 = wbl.begin(), End = wbl.end();
            it1 != End;
              ++it1)
        {
            result[i].push_back((*it1).boxid());
        }
        ++cit;
    }

    for (int i  = 0; i < nprocs; i++)
        delete vbbs[i];
}

void
DistributionMapping::KnapSackDoIt (const std::vector<long>& wgts,
                                   int                      nprocs,
                                   double&                  efficiency,
                                   bool                     do_full_knapsack)
{
    BL_PROFILE("DistributionMapping::KnapSackDoIt()");

    std::vector< std::vector<int> > vec;

    efficiency = 0;

    knapsack(wgts,nprocs,vec,efficiency,do_full_knapsack);

    BL_ASSERT(vec.size() == nprocs);

    Array<long> wgts_per_cpu(nprocs,0);

    for (unsigned int i = 0, N = vec.size(); i < N; i++)
    {
        for (std::vector<int>::iterator lit = vec[i].begin(), End = vec[i].end();
             lit != End;
             ++lit)
        {
            wgts_per_cpu[i] += wgts[*lit];
        }
    }

    std::vector<LIpair> LIpairV;

    LIpairV.reserve(nprocs);

    for (int i = 0; i < nprocs; i++)
    {
        LIpairV.push_back(LIpair(wgts_per_cpu[i],i));
    }

    Sort(LIpairV, true);

    Array<int> ord;

    LeastUsedCPUs(nprocs,ord);

    for (unsigned int i = 0, N = vec.size(); i < N; i++)
    {
        const int idx = LIpairV[i].second;
        const int cpu = ord[i];

        for (std::vector<int>::iterator lit = vec[idx].begin(), End = vec[idx].end();
             lit != End;
             ++lit)
        {
            m_ref->m_pmap[*lit] = cpu;
        }
    }
    //
    // Set sentinel equal to our processor number.
    //
    m_ref->m_pmap[wgts.size()] = ParallelDescriptor::MyProc();

    if (verbose && ParallelDescriptor::IOProcessor())
    {
        std::cout << "KNAPSACK efficiency: " << efficiency << '\n';
    }

}

void
DistributionMapping::KnapSackProcessorMap (const std::vector<long>& wgts,
                                           int                      nprocs,
                                           double*                  efficiency,
                                           bool                     do_full_knapsack)
{
    BL_ASSERT(wgts.size() > 0);

    if (m_ref->m_pmap.size() !=  wgts.size() + 1)
    {
        m_ref->m_pmap.resize(wgts.size() + 1);
    }

    if (wgts.size() <= nprocs || nprocs < 2)
    {
        RoundRobinProcessorMap(wgts.size(),nprocs);

        if (efficiency) *efficiency = 1;
    }
    else
    {
        double eff = 0;
        KnapSackDoIt(wgts, nprocs, eff, do_full_knapsack);
        if (efficiency) *efficiency = eff;
    }
}

void
DistributionMapping::KnapSackProcessorMap (const BoxArray& boxes,
					   int             nprocs)
{
    BL_ASSERT(boxes.size() > 0);
    BL_ASSERT(m_ref->m_pmap.size() == boxes.size()+1);

    if (boxes.size() <= nprocs || nprocs < 2)
    {
        RoundRobinProcessorMap(boxes,nprocs);
    }
    else
    {
        std::vector<long> wgts(boxes.size());

        for (unsigned int i = 0, N = boxes.size(); i < N; i++)
            wgts[i] = boxes[i].numPts();

        double effi = 0;
        bool do_full_knapsack = true;
        KnapSackDoIt(wgts, nprocs, effi, do_full_knapsack);
    }

if(ParallelDescriptor::IOProcessor()) {
  Array<long> ncells(nprocs, 0);
  for(int i(0); i < m_ref->m_pmap.size() - 1; ++i) {
    int index(m_ref->m_pmap[i]);
    ncells[index] += boxes[index].numPts();
  }
  static int count(0);
  std::stringstream dfss;
  dfss << "KSncells.count_" << count++ << ".xgr";
  std::ofstream bos(dfss.str().c_str());
  for(int i(0); i < ncells.size(); ++i) {
    bos << i << ' ' << ncells[i] << '\n';
  }
  bos.close();
}
/*
*/

}

namespace
{
    struct SFCToken
    {
        class Compare
        {
        public:
            bool operator () (const SFCToken& lhs,
                              const SFCToken& rhs) const;
        };

        SFCToken (int box, const IntVect& idx, Real vol)
            :
            m_box(box), m_idx(idx), m_vol(vol) {}

        int     m_box;
        IntVect m_idx;
        Real    m_vol;

        static int MaxPower;
    };
}

int SFCToken::MaxPower = 64;

bool
SFCToken::Compare::operator () (const SFCToken& lhs,
                                const SFCToken& rhs) const
{
    for (int i = SFCToken::MaxPower - 1; i >= 0; --i)
    {
        const int N = (1<<i);

        for (int j = BL_SPACEDIM-1; j >= 0; --j)
        {
            const int il = lhs.m_idx[j]/N;
            const int ir = rhs.m_idx[j]/N;

            if (il < ir)
            {
                return true;
            }
            else if (il > ir)
            {
                return false;
            }
        }
    }
    return false;
}

static
void
Distribute (const std::vector<SFCToken>&     tokens,
            int                              nprocs,
            Real                             volpercpu,
            std::vector< std::vector<int> >& v)

{
    BL_ASSERT(v.size() == nprocs);

    int  K        = 0;
    Real totalvol = 0;

    for (int i = 0; i < nprocs; ++i)
    {
        int  cnt = 0;
        Real vol = 0;

        for ( int TSZ = tokens.size();
              K < TSZ && (i == (nprocs-1) || vol < volpercpu);
              ++cnt, ++K)
        {
            vol += tokens[K].m_vol;

            v[i].push_back(tokens[K].m_box);
        }

        totalvol += vol;

        if ((totalvol/(i+1)) > volpercpu &&
            cnt > 1                      &&
            K < tokens.size())
        {
            --K;
            v[i].pop_back();
            totalvol -= tokens[K].m_vol;
        }
    }

#ifndef NDEBUG
    int cnt = 0;
    for (int i = 0; i < nprocs; ++i)
        cnt += v[i].size();
    BL_ASSERT(cnt == tokens.size());
#endif
}

void
DistributionMapping::SFCProcessorMapDoIt (const BoxArray&          boxes,
                                          const std::vector<long>& wgts,
                                          int                      nprocs)
{
//BoxLib::Abort("SFCDoIt");
    BL_PROFILE("DistributionMapping::SFCProcessorMapDoIt()");

    std::vector<SFCToken> tokens;

    const int N = boxes.size();

    tokens.reserve(N);

    int maxijk = 0;

    for (int i = 0; i < N; ++i)
    {
        tokens.push_back(SFCToken(i,boxes[i].smallEnd(),wgts[i]));

        const SFCToken& token = tokens.back();

        D_TERM(maxijk = std::max(maxijk, token.m_idx[0]);,
               maxijk = std::max(maxijk, token.m_idx[1]);,
               maxijk = std::max(maxijk, token.m_idx[2]););
    }
    //
    // Set SFCToken::MaxPower for BoxArray.
    //
    int m = 0;
    for ( ; (1 << m) <= maxijk; ++m) {
        ;  // do nothing
    }
    SFCToken::MaxPower = m;
    //
    // Put'm in Morton space filling curve order.
    //
    std::sort(tokens.begin(), tokens.end(), SFCToken::Compare());
    //
    // Split'm up as equitably as possible per CPU.
    //
    Real volpercpu = 0;
    for (int i = 0, N = tokens.size(); i < N; ++i)
        volpercpu += tokens[i].m_vol;
    volpercpu /= nprocs;

    std::vector< std::vector<int> > vec(nprocs);

    Distribute(tokens,nprocs,volpercpu,vec);

    tokens.clear();

    Array<long> wgts_per_cpu(nprocs,0);

    for (unsigned int i = 0, N = vec.size(); i < N; ++i)
    {
        const std::vector<int>& vi = vec[i];

        for (int j = 0, M = vi.size(); j < M; ++j)
            wgts_per_cpu[i] += wgts[vi[j]];
    }

    std::vector<LIpair> LIpairV;

    LIpairV.reserve(nprocs);

    for (int i = 0; i < nprocs; ++i)
    {
        LIpairV.push_back(LIpair(wgts_per_cpu[i],i));
    }

    Sort(LIpairV, true);

    Array<int> ord;

    LeastUsedCPUs(nprocs,ord);

    for (int i = 0; i < nprocs; ++i)
    {
        const int cpu = ord[i];
        const int idx = LIpairV[i].second;

        const std::vector<int>& vi = vec[idx];

        for (int j = 0, N = vi.size(); j < N; ++j)
        {
            m_ref->m_pmap[vi[j]] = cpu;
        }
    }
    //
    // Set sentinel equal to our processor number.
    //
    m_ref->m_pmap[boxes.size()] = ParallelDescriptor::MyProc();

    if (verbose && ParallelDescriptor::IOProcessor())
    {
        Real sum_wgt = 0, max_wgt = 0;
        for (int i = 0, N = wgts_per_cpu.size(); i < N; ++i)
        {
            const long W = wgts_per_cpu[i];
            if (W > max_wgt)
                max_wgt = W;
            sum_wgt += W;
        }

        std::cout << "SFC efficiency: " << (sum_wgt/(nprocs*max_wgt)) << '\n';
    }
}

void
DistributionMapping::SFCProcessorMap (const BoxArray& boxes,
                                      int             nprocs)
{
    BL_ASSERT(boxes.size() > 0);

    if (m_ref->m_pmap.size() != boxes.size() + 1)
    {
        m_ref->m_pmap.resize(boxes.size()+1);
    }

    if (boxes.size() < sfc_threshold*nprocs)
    {
        KnapSackProcessorMap(boxes,nprocs);
    }
    else
    {
        std::vector<long> wgts;

        wgts.reserve(boxes.size());

        for (BoxArray::const_iterator it = boxes.begin(), End = boxes.end(); it != End; ++it)
        {
            wgts.push_back(it->volume());
        }

        SFCProcessorMapDoIt(boxes,wgts,nprocs);
    }
}

void
DistributionMapping::SFCProcessorMap (const BoxArray&          boxes,
                                      const std::vector<long>& wgts,
                                      int                      nprocs)
{
    BL_ASSERT(boxes.size() > 0);
    BL_ASSERT(boxes.size() == wgts.size());

    if (m_ref->m_pmap.size() != wgts.size() + 1)
    {
        m_ref->m_pmap.resize(wgts.size()+1);
    }

    if (boxes.size() < sfc_threshold*nprocs)
    {
        KnapSackProcessorMap(wgts,nprocs);
    }
    else
    {
        SFCProcessorMapDoIt(boxes,wgts,nprocs);
    }
}


namespace
{
    struct PFCToken
    {
        class Compare
        {
        public:
            bool operator () (const PFCToken& lhs,
                              const PFCToken& rhs) const;
        };

        PFCToken (int box, const IntVect& idx, Real vol)
            :
            m_box(box), m_idx(idx), m_vol(vol) {}

        int     m_box;
        IntVect m_idx;
        Real    m_vol;
    };
}



bool
PFCToken::Compare::operator () (const PFCToken& lhs,
                                const PFCToken& rhs) const
{
  return lhs.m_idx.lexLT(rhs.m_idx);
}


void
DistributionMapping::CurrentBytesUsed (int nprocs, Array<long>& result)
{
    result.resize(nprocs);
    Array<long> bytes(nprocs, 0);

#ifdef BL_USE_MPI
    BL_PROFILE("DistributionMapping::CurrentBytesUsed()");


    MPI_Allgather(&BoxLib::total_bytes_allocated_in_fabs,
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  bytes.dataPtr(),
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  ParallelDescriptor::Communicator());
#endif

    for (int i(0); i < nprocs; ++i)
    {
        result[i] = bytes[i];
    }
if(ParallelDescriptor::IOProcessor()) {
  std::cout << "**********************************" << std::endl;
  for(int i(0); i < result.size(); ++i) {
    std::cout << "currentBytes[" << i << "] = " << result[i] << std::endl;
  }
  std::cout << "**********************************" << std::endl;
  static int count(0);
  std::stringstream dfss;
  dfss << "CurrentBytes.count_" << count++ << ".xgr";
  std::ofstream bos(dfss.str().c_str());
  for(int i(0); i < result.size(); ++i) {
    bos << i << ' ' << result[i] << '\n';
  }
  bos.close();
}

}


void
DistributionMapping::CurrentCellsUsed (int nprocs, Array<long>& result)
{
    result.resize(nprocs);
    Array<long> cells(nprocs, 0);

#ifdef BL_USE_MPI
    BL_PROFILE("DistributionMapping::CurrentCellsUsed()");


    MPI_Allgather(&BoxLib::total_cells_allocated_in_fabs,
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  cells.dataPtr(),
                  1,
                  ParallelDescriptor::Mpi_typemap<long>::type(),
                  ParallelDescriptor::Communicator());
#endif

    for(int i(0); i < nprocs; ++i) {
      result[i] = cells[i];
    }
if(ParallelDescriptor::IOProcessor()) {
  std::cout << "**********************************" << std::endl;
  for(int i(0); i < result.size(); ++i) {
    std::cout << "currentCells[" << i << "] = " << result[i] << std::endl;
  }
  /*
  std::cout << "**********************************" << std::endl;
  static int count(0);
  std::stringstream dfss;
  dfss << "CurrentCells.count_" << count++ << ".xgr";
  std::ofstream bos(dfss.str().c_str());
  for(int i(0); i < result.size(); ++i) {
    bos << i << ' ' << result[i] << '\n';
  }
  bos.close();
  */
}

}


static
void
Distribute (const std::vector<PFCToken>&     tokens,
            int                              nprocs,
            Real                             volpercpu,
            std::vector< std::vector<int> >& v)

{
BoxLib::Abort("PFC Distribute not used.");
/*
    BL_ASSERT(v.size() == nprocs);

    int  K        = 0;
    Real totalvol = 0;
    const int Navg = tokens.size() / nprocs;

    for (int i = 0; i < nprocs; ++i) {
        int  cnt = 0;
        Real vol = 0;
        v[i].reserve(Navg + 2);
        for ( int TSZ = tokens.size();
              K < TSZ && (i == (nprocs-1) || vol < volpercpu);
              cnt++, K++)
        {
            vol += tokens[K].m_vol;
            v[i].push_back(tokens[K].m_box);
        }

        totalvol += vol;
        if ((totalvol/(i+1)) > volpercpu &&
            cnt > 1                      &&
            K < tokens.size())
        {
            --K;
            v[i].pop_back();
            totalvol -= tokens[K].m_vol;
        }
    }

#ifndef NDEBUG
    int cnt = 0;
    for (int i = 0; i < nprocs; ++i) {
        cnt += v[i].size();
    }
    BL_ASSERT(cnt == tokens.size());
#endif
*/
}


void
DistributionMapping::PFCProcessorMapDoIt (const BoxArray&          boxes,
                                          const std::vector<long>& wgts,
                                          int                      nprocs)
{
    BL_PROFILE("DistributionMapping::PFCProcessorMapDoIt()");

    std::vector< std::vector<int> > vec(nprocs);
    std::vector<PFCToken> tokens;
    tokens.reserve(boxes.size());
    int maxijk(0);

    for(int i(0), N(boxes.size()); i < N; ++i) {
        tokens.push_back(PFCToken(i, boxes[i].smallEnd(), wgts[i]));
        const PFCToken &token = tokens.back();
        D_TERM(maxijk = std::max(maxijk, token.m_idx[0]);,
               maxijk = std::max(maxijk, token.m_idx[1]);,
               maxijk = std::max(maxijk, token.m_idx[2]););
    }

    std::sort(tokens.begin(), tokens.end(), PFCToken::Compare());  // sfc order

    Array<long> aCurrentBytes;
    CurrentBytesUsed(nprocs, aCurrentBytes);
    Real totB(0.0), totC(0.0), bytesPerCell(0.0);
    Array<long> aCurrentCells;
    CurrentCellsUsed(nprocs, aCurrentCells);
    if(ParallelDescriptor::IOProcessor()) {
      for(int i(0); i < aCurrentCells.size(); ++i) {
        std::cout << "aCurrentCells[" << i << "] = " << aCurrentCells[i] << std::endl;
	totC += aCurrentCells[i];
	totB += aCurrentBytes[i];
      }
      if(totC > 0.0) {
        std::cout << "BytesPerCell = " << totB / totC << std::endl;
      } else {
        std::cout << "BytesPerCell = " << 0.0 << std::endl;
      }
    }

    long totalCurrentCells(0);
    for(int i(0); i < nprocs; ++i) {
      totalCurrentCells += aCurrentCells[i];
    }
    Real avgCurrentCells(static_cast<Real>(totalCurrentCells) / nprocs);

      // ===============================   // Distribute(tokens,nprocs,volpercpu,vec);
      int  K(0);
      Real totalvol(0.0), volpercpu(0.0), ccScale(1.0);
      const int Navg(tokens.size() / nprocs);
      long totalNewCells(0);
      long totalNewCellsB(0);
      for(int i(0); i < tokens.size(); ++i) {        // new cells to add
        totalNewCells += tokens[i].m_vol;
        totalNewCellsB += boxes[i].numPts();
      }
      if(totalNewCells != totalNewCellsB) {
        BoxLib::Abort("tnc");
      }
      volpercpu = static_cast<Real>(totalNewCells) / nprocs;

      Array<long> scaledCurrentCells(aCurrentCells.size(), 0);
      if(totalCurrentCells > 0) {
        ccScale = static_cast<Real>(totalNewCells) / totalCurrentCells;
      }
      for(int i(0); i < aCurrentCells.size(); ++i) {
        scaledCurrentCells[i] = ccScale * aCurrentCells[i];
      }

      Array<long> newVolPerCPU(nprocs, 0);
      /*
      if(totalCurrentCells > 0) {
        for(int i(0); i < newVolPerCPU.size(); ++i) {
          newVolPerCPU[i] = (2.0 * volpercpu) - scaledCurrentCells[i];
        }
      } else {
        for(int i(0); i < newVolPerCPU.size(); ++i) {
          newVolPerCPU[i] = volpercpu;
        }
      }
      */

long accDiff(0), accNVPC(0), accAV(0);
KnapSackProcessorMap(boxes, nprocs);
for(int i(0); i < m_ref->m_pmap.size() - 1; ++i) {
  int whichProc(m_ref->m_pmap[i]);
  newVolPerCPU[whichProc] += boxes[i].numPts();
}
for(int i(0); i < nprocs; ++i) {
  accNVPC += newVolPerCPU[i];
}

      if(ParallelDescriptor::IOProcessor()) {
        std::cout << "_here 1 totalCurrentCells = " << totalCurrentCells << std::endl;
        std::cout << "_here 1 totalNewCells     = " << totalNewCells << std::endl;
        std::cout << "_here 1 ccScale           = " << ccScale << std::endl;
        std::cout << "_here 1 volpercpu         = " << volpercpu << std::endl;
        std::cout << "_here 1 boxes.size()      = " << boxes.size() << std::endl;
        for(int i(0); i < newVolPerCPU.size(); ++i) {
          std::cout << "_here 1.1:  newVolPerCPU[" << i << "] diff = "
	        << newVolPerCPU[i] << "  " << volpercpu - newVolPerCPU[i] << std::endl;
        }
      }
      for(int i(0); i < nprocs; ++i) {
        int  cnt(0);
        Real vol(0.0);
	long accVol(0), oldAccVol(0);
        long oldCells(aCurrentCells[i]);
	long halfVol;
        vec[i].reserve(Navg + 2);

        for(int TSZ(tokens.size()); K < TSZ &&
	    (i == (nprocs-1) || vol < (newVolPerCPU[i] - 0));
            ++cnt, ++K)
        {
            vol += tokens[K].m_vol;
            accVol += tokens[K].m_vol;
	    oldAccVol = accVol;
            vec[i].push_back(tokens[K].m_box);
	    halfVol = tokens[K].m_vol / 2;
        }

        totalvol += vol;
        if((totalvol / (i + 1)) > (newVolPerCPU[i] + 0) && cnt > 1 && K < tokens.size()) {
            --K;
            vec[i].pop_back();
            totalvol -= tokens[K].m_vol;
	    oldAccVol = accVol;
            accVol -= tokens[K].m_vol;
        }
      aCurrentCells[i] += accVol;

      accAV   += accVol;

if(ParallelDescriptor::IOProcessor()) {
  accDiff += newVolPerCPU[i] - accVol;
  std::cout << "_here 2:  proc nVPC accVol diff odiff accDiff :::: " << i << ":  "
	    << newVolPerCPU[i] << "  -   " << accVol << " =  "
	    << newVolPerCPU[i] - accVol << "  "
	    << newVolPerCPU[i] - oldAccVol << "  " << accDiff << std::endl;
}

      }
      // ===============================

if(ParallelDescriptor::IOProcessor()) {
  long npoints(0);
  for(int i(0); i < boxes.size(); ++i) {
    npoints += boxes[i].numPts();
  }
  std::cout << "_here 2.1:  accNVPC accAV npoints = " << accNVPC << "  "
            << accAV << "  " << npoints << std::endl;
  std::cout << "_here 3:  vvvvvvvvvvvvvvvvvvvvvvvv after dist" << std::endl;
  for(int i(0); i < aCurrentCells.size(); ++i) {
    std::cout << "aCurrentCells[" << i << "] = " << aCurrentCells[i] << std::endl;
  }
  std::cout << "_here 3:  ^^^^^^^^^^^^^^^^^^^^^^^^ after dist" << std::endl;
}

    tokens.clear();
    Array<long> wgts_per_cpu(nprocs, 0);
    for (unsigned int i(0), N(vec.size()); i < N; ++i) {
        const std::vector<int>& vi = vec[i];
        for (int j(0), M(vi.size()); j < M; ++j) {
            wgts_per_cpu[i] += wgts[vi[j]];
	}
    }

    //std::vector<LIpair> LIpairV;
    //LIpairV.reserve(nprocs);
    //for (int i(0); i < nprocs; ++i) {
        //LIpairV.push_back(LIpair(wgts_per_cpu[i],i));
    //}
    //Sort(LIpairV, true);

    for (int i(0); i < nprocs; ++i) {
        //const int cpu = i;
        //const int idx = LIpairV[i].second;
        const std::vector<int>& vi = vec[i];

        for(int j(0), N(vi.size()); j < N; ++j) {
          m_ref->m_pmap[vi[j]] = ProximityMap(i);
        }
    }

    // Set sentinel equal to our processor number.
    m_ref->m_pmap[boxes.size()] = ParallelDescriptor::MyProc();

    if(ParallelDescriptor::IOProcessor()) {
        Real sum_wgt = 0, max_wgt = 0;
        for(int i = 0, N = wgts_per_cpu.size(); i < N; ++i) {
            const long W = wgts_per_cpu[i];
            if(W > max_wgt) {
              max_wgt = W;
	    }
            sum_wgt += W;
        }
        std::cout << "PFC efficiency: " << (sum_wgt/(nprocs*max_wgt)) << '\n';
    }
/*
if(ParallelDescriptor::IOProcessor()) {
  static int count(0);
  std::stringstream dfss;
  dfss << "CurrentCellsAcc.count_" << count++ << ".xgr";
  std::ofstream bos(dfss.str().c_str());
  for(int i(0); i < aCurrentCells.size(); ++i) {
    bos << i << ' ' << aCurrentCells[i] << '\n';
  }
  bos.close();
}
*/


if(ParallelDescriptor::IOProcessor()) {
  Array<long> ncells(nprocs, 0);
  for(int i(0); i < m_ref->m_pmap.size() - 1; ++i) {
    int index(m_ref->m_pmap[i]);
    ncells[index] += boxes[index].numPts();
  }
  static int count(0);
  std::stringstream dfss;
  dfss << "PFCncells.count_" << count++ << ".xgr";
  std::ofstream bos(dfss.str().c_str());
  for(int i(0); i < ncells.size(); ++i) {
    bos << i << ' ' << ncells[i] << '\n';
  }
  bos.close();
}

}


void
DistributionMapping::PFCProcessorMap (const BoxArray& boxes,
                                      int             nprocs)
{
if(ParallelDescriptor::IOProcessor()) {
  std::cout << "PFCProcessorMap(ba, n) ###########" << std::endl;
}
    BL_ASSERT(boxes.size() > 0);

    if (m_ref->m_pmap.size() != boxes.size() + 1) {
        m_ref->m_pmap.resize(boxes.size()+1);
    }

    std::vector<long> wgts;
    wgts.reserve(boxes.size());

    for (BoxArray::const_iterator it = boxes.begin(), End = boxes.end(); it != End; ++it)
    {
      wgts.push_back(it->numPts());
    }
    PFCProcessorMapDoIt(boxes,wgts,nprocs);
}


void
DistributionMapping::PFCProcessorMap (const BoxArray&          boxes,
                                      const std::vector<long>& wgts,
                                      int                      nprocs)
{
if(ParallelDescriptor::IOProcessor()) {
  std::cout << "PFCProcessorMap(ba, w, n) ###########" << std::endl;
}
    BL_ASSERT(boxes.size() > 0);
    BL_ASSERT(boxes.size() == wgts.size());

    if (m_ref->m_pmap.size() != wgts.size() + 1) {
        m_ref->m_pmap.resize(wgts.size()+1);
    }
    PFCProcessorMapDoIt(boxes,wgts,nprocs);
}


std::string
DistributionMapping::GetProcName() {
  int resultLen(-1);
  char cProcName[MPI_MAX_PROCESSOR_NAME + 11];
#ifdef BL_USE_MPI
  MPI_Get_processor_name(cProcName, &resultLen);
#endif
  if(resultLen < 1) {
    strcpy(cProcName, "NoProcName");
  }
  return(std::string(cProcName));
}


int
DistributionMapping::GetProcNumber() {
#ifdef BL_HOPPER
  std::string procName(GetProcName());
  return(atoi(procName.substr(3, string::npos).c_str()));
#else
#ifdef BL_SIM_HOPPER
  //static int procNumber = (100 * ParallelDescriptor::MyProc()) % 6527;
  static int procNumber = ParallelDescriptor::MyProc();
  std::cout << ParallelDescriptor::MyProc() << "||procNumber = " << procNumber << std::endl;
  return(procNumber);
#else
  return(ParallelDescriptor::MyProc());
#endif
#endif
}


void
DistributionMapping::InitProximityMap()
{
  int nProcs(ParallelDescriptor::NProcs());
  int procNumber(GetProcNumber());
  Array<int> procNumbers(nProcs, -1);

  proximityMap.resize(ParallelDescriptor::NProcs(), 0);
  proximityOrder.resize(ParallelDescriptor::NProcs(), 0);

#ifdef BL_USE_MPI
  MPI_Allgather(&procNumber, 1, ParallelDescriptor::Mpi_typemap<int>::type(),
                procNumbers.dataPtr(), 1, ParallelDescriptor::Mpi_typemap<int>::type(),
                ParallelDescriptor::Communicator());
#endif

  for(int i(0); i < procNumbers.size(); ++i) {
    pNumRankMM.insert(std::pair<int, int>(procNumbers[i], i));
    rankPNumMap.insert(std::pair<int, int>(i, procNumbers[i]));
  }

  // order ranks by procNumber
  Array<int> pNumOrderRank(nProcs, -1);
  int pnor(0);
  for(std::multimap<int, int>::iterator mmit = pNumRankMM.begin();
      mmit != pNumRankMM.end(); ++mmit)
  {
    pNumOrderRank[pnor++] = mmit->second;
  }

  if(ParallelDescriptor::IOProcessor())
  {
    Box tBox;
    FArrayBox tFab;
#ifdef BL_SIM_HOPPER
    std::ifstream ifs("topolcoords.simhopper.3d.fab");
#else
    std::ifstream ifs("topolcoords.3d.fab");
#endif
    if( ! ifs.good())
    {
      std::cerr << "**** Error in DistributionMapping::InitProximityMap():  "
                << "cannot open topolcoords.3d.fab" << std::endl;
      // set a reasonable default

    } else {
      tFab.readFrom(ifs);
      ifs.close();
      tBox = tFab.box();
      std::cout << "tBox = " << tBox << "  ncomp = " << tFab.nComp() << std::endl;
/*
if(ParallelDescriptor::IOProcessor()) {
FArrayBox nodeFab(tBox, 2);
nodeFab.setVal(-1.0);
int i(0);
for(IntVect iv(tBox.smallEnd()); iv <= tBox.bigEnd(); tBox.next(iv)) {
  nodeFab(iv, 0) = i++;
  nodeFab(iv, 1) = i++;
}
std::ofstream osNodeFab("topolcoords.simhopper.3d.fab");
nodeFab.writeOn(osNodeFab);
osNodeFab.close();
}
*/


      for(int nc(0); nc < tFab.nComp(); ++nc) {
        for(IntVect iv(tBox.smallEnd()); iv <= tBox.bigEnd(); tBox.next(iv)) {
          int pnum(tFab(iv, nc));
          if(pnum >= 0) {
            //std::cout << ">>>> iv pnum = " << iv << "  " << pnum << std::endl;
            pNumTopIVMap.insert(std::pair<int, IntVect>(pnum, iv));
	    topIVpNumMM.insert(std::pair<IntVect, int>(iv, pnum));
          }
        }
      }

      // ------------------------------- make sfc from tFab
      std::vector<SFCToken> tFabTokens;  // use SFCToken here instead of PFC
      tFabTokens.reserve(tBox.numPts());
      int maxijk(0);

      int i(0);
      for(IntVect iv(tBox.smallEnd()); iv <= tBox.bigEnd(); tBox.next(iv))
      {
          tFabTokens.push_back(SFCToken(i++, iv, 1.0));
          const SFCToken &token = tFabTokens.back();

          D_TERM(maxijk = std::max(maxijk, token.m_idx[0]);,
                 maxijk = std::max(maxijk, token.m_idx[1]);,
                 maxijk = std::max(maxijk, token.m_idx[2]););
      }
      // Set SFCToken::MaxPower for BoxArray.
      int m(0);
      for ( ; (1<<m) <= maxijk; m++)
      {
        // do nothing
      }
      SFCToken::MaxPower = m;
      std::sort(tFabTokens.begin(), tFabTokens.end(), SFCToken::Compare());  // sfc order
      FArrayBox tFabSFC(tBox, 1);
      tFabSFC.setVal(-1.0);
      for(int i(0); i < tFabTokens.size(); ++i)
      {
	IntVect &iv = tFabTokens[i].m_idx;
        tFabSFC(iv) = i;
      }
      std::ofstream tfofs("tFabSFC.3d.fab");
      tFabSFC.writeOn(tfofs);
      tfofs.close();
      // ------------------------------- end make sfc from tFab

      // ------------------------------- order ranks by topological sfc
      std::vector<IntVect> nodesSFC;
      std::cout << std::endl << "----------- order ranks by topological sfc" << std::endl;
      for(int i(0); i < tFabTokens.size(); ++i) {
        IntVect &iv = tFabTokens[i].m_idx;
        std::vector<int> ivRanks = RanksFromTopIV(iv);
        if(ivRanks.size() > 0) {
          nodesSFC.push_back(iv);
          std::cout << "---- iv ranks = " << iv << "  ";
          for(int ivr(0); ivr < ivRanks.size(); ++ivr) {
            ranksSFC.push_back(ivRanks[ivr]);
            std::cout << ivRanks[ivr] << "  ";
          }
          std::cout << std::endl;
        }
      }
      if(ranksSFC.size() != nProcs) {
        std::cerr << "**** Error:  ranksSFC.size() != nProcs:  " << ranksSFC.size()
                  << "  " <<  nProcs << std::endl;
      }
      std::cout << "++++++++++++++++++++++++" << std::endl;
      if(proximityMap.size() != ParallelDescriptor::NProcs()) {
	std::cout << "####::InitProximityMap: proximityMap not resized yet." << std::endl;
        proximityMap.resize(ParallelDescriptor::NProcs(), 0);
        proximityOrder.resize(ParallelDescriptor::NProcs(), 0);
      }
      for(int i(0); i < ranksSFC.size(); ++i) {
        std::cout << "++++ rank ranksSFC = " << i << "  " << ranksSFC[i] << std::endl;
	proximityMap[i] = ranksSFC[i];
      }
      std::map<int, int> proximityOrderMap;  // [proximityMap[rank], rank]
      for(int i(0); i < proximityMap.size(); ++i) {
	proximityOrderMap.insert(std::pair<int, int>(proximityMap[i], i));
      }
      for(std::map<int, int>::iterator it = proximityOrderMap.begin();
          it != proximityOrderMap.end(); ++it)
      {
        proximityOrder[it->first] = it->second;
      }
      for(int i(0); i < proximityOrder.size(); ++i) {
        std::cout << "++++ rank proximityOrder = " << i << "  " << proximityOrder[i] << std::endl;
      }
      std::cout << "----------- end order ranks by topological sfc" << std::endl;
    }
    FArrayBox nodeFab(tBox);
    nodeFab.setVal(-nProcs);
    for(int i(0); i < nProcs; ++i) {
      IntVect iv = DistributionMapping::TopIVFromRank(i);
      nodeFab(iv) = i;  // this overwrites previous ones
      std::cout << "rank pNum topiv = " << i << "  "
                << DistributionMapping::ProcNumberFromRank(i) << "  " << iv << std::endl;
    }
    std::ofstream osNodeFab("nodes.3d.fab");
    nodeFab.writeOn(osNodeFab);
    osNodeFab.close();
  }

  ParallelDescriptor::Bcast(proximityMap.dataPtr(), proximityMap.size(),
                            ParallelDescriptor::IOProcessorNumber());
  ParallelDescriptor::Bcast(proximityOrder.dataPtr(), proximityOrder.size(),
                            ParallelDescriptor::IOProcessorNumber());
}


int
DistributionMapping::NHops(const Box &tbox, const IntVect &ivfrom, const IntVect &ivto)
{
  int nhops(0);
  for(int d(0); d < BL_SPACEDIM; ++d) {
    int bl(tbox.length(d));
    int ivl(std::min(ivfrom[d], ivto[d]));
    int ivh(std::max(ivfrom[d], ivto[d]));
    int dist(std::min(ivh - ivl, ivl + bl - ivh));
    nhops += dist;
  }
  return nhops;
}


int
DistributionMapping::ProcNumberFromRank(const int rank) {
  int procnum(-1);
  std::map<int, int>::iterator it = rankPNumMap.find(rank);
  if(it == rankPNumMap.end()) {
    if(ParallelDescriptor::IOProcessor()) {
      std::cerr << "**** Error in ProcNumberFromRank:  rank not found:  "
                << rank << std::endl;
    }
  } else {
    procnum = it->second;
    if(procnum != rankPNumMap[rank]) {
      std::cerr << "**** Error in ProcNumberFromRank:  rank not matched:  "
                << rank << std::endl;
    }
  }
  return procnum;
}


std::vector<int>
DistributionMapping::RanksFromProcNumber(const int procnum) {
  std::vector<int> ranks;
  std::pair<std::multimap<int, int>::iterator, std::multimap<int, int>::iterator> mmiter;
  mmiter = pNumRankMM.equal_range(procnum);
  for(std::multimap<int, int>::iterator it = mmiter.first; it != mmiter.second; ++it) {
    ranks.push_back(it->second);
  }
  return ranks;
}


IntVect
DistributionMapping::TopIVFromProcNumber(const int procnum) {
  IntVect iv;
  std::map<int, IntVect>::iterator it = pNumTopIVMap.find(procnum);
  if(it == pNumTopIVMap.end()) {
    if(ParallelDescriptor::IOProcessor()) {
      std::cerr << "**** Error in TopIVFromProcNumber:  procnum not found:  "
                << procnum << std::endl;
    }
  } else {
    iv = it->second;
    if(iv != pNumTopIVMap[procnum]) {
      std::cerr << "**** Error in TopIVFromProcNumber:  procnum not matched:  "
                << procnum << std::endl;
    }
  }
  return iv;
}


std::vector<int>
DistributionMapping::ProcNumbersFromTopIV(const IntVect &iv) {
  std::vector<int> pnums;
  std::pair<std::multimap<IntVect, int, IntVect::Compare>::iterator,
            std::multimap<IntVect, int, IntVect::Compare>::iterator> mmiter;
  mmiter = topIVpNumMM.equal_range(iv);
  for(std::multimap<IntVect, int, IntVect::Compare>::iterator it = mmiter.first;
      it != mmiter.second; ++it)
  {
    pnums.push_back(it->second);
  }
  return pnums;
}


IntVect
DistributionMapping::TopIVFromRank(const int rank) {
  return TopIVFromProcNumber(ProcNumberFromRank(rank));
}


std::vector<int>
DistributionMapping::RanksFromTopIV(const IntVect &iv) {
  std::vector<int> ranks;
  std::vector<int> pnums = ProcNumbersFromTopIV(iv);
  for(int i(0); i < pnums.size(); ++i) {
    std::vector<int> rfpn = RanksFromProcNumber(pnums[i]);
    for(int r(0); r < rfpn.size(); ++r) {
      ranks.push_back(rfpn[r]);
    }
  }
  return ranks;
}


void
DistributionMapping::CacheStats (std::ostream& os)
{
    if (ParallelDescriptor::IOProcessor() && m_Cache.size())
    {
        os << "DistributionMapping::m_Cache.size() = "
           << m_Cache.size()
           << " [ (refs,size): ";

        for (std::map< int,LnClassPtr<Ref> >::const_iterator it = m_Cache.begin();
             it != m_Cache.end();
             ++it)
        {
            os << '(' << it->second.linkCount() << ',' << it->second->m_pmap.size()-1 << ") ";
        }

        os << "]\n";
    }
}


void
DistributionMapping::PrintDiagnostics(const std::string &filename)
{
    int nprocs(ParallelDescriptor::NProcs());
    Array<long> bytes(nprocs, 0);

    ParallelDescriptor::Gather(&BoxLib::total_bytes_allocated_in_fabs,
                               1,
                               bytes.dataPtr(),
                               1,
                               ParallelDescriptor::IOProcessorNumber());

    if(ParallelDescriptor::IOProcessor()) {
      std::ofstream bos(filename.c_str());
      for(int i(0); i < nprocs; ++i) {
        bos << i << ' ' << bytes[i] << '\n';
      }
      bos.close();
      //std::string TBP("TBP_" + filename);
      //std::ofstream tos(TBP.c_str());
      //for(int i(0); i < totalBoxPoints.size(); ++i) {
        //tos << i << ' ' << totalBoxPoints[i] << '\n';
      //}
      //tos.close();
    }
    ParallelDescriptor::Barrier();
}


std::ostream&
operator<< (std::ostream&              os,
            const DistributionMapping& pmap)
{
    os << "(DistributionMapping" << '\n';
    //
    // Do not print the sentinel value.
    //
    for (int i = 0; i < pmap.ProcessorMap().size() - 1; i++)
    {
        os << "m_pmap[" << i << "] = " << pmap.ProcessorMap()[i] << '\n';
    }

    os << ')' << '\n';

    if (os.fail())
        BoxLib::Error("operator<<(ostream &, DistributionMapping &) failed");

    return os;
}
