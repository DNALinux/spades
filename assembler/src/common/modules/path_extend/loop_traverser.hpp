//***************************************************************************
//* Copyright (c) 2015 Saint Petersburg State University
//* Copyright (c) 2011-2014 Saint Petersburg Academic University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#ifndef LOOP_TRAVERSER_H_
#define LOOP_TRAVERSER_H_

#include "path_extender.hpp"
#include "pe_resolver.hpp"
#include "path_visualizer.hpp"

#include "assembly_graph/core/graph.hpp"

namespace path_extend {

class LoopTraverser {
    const Graph& g_;
    const GraphCoverageMap& cov_map_;
    const size_t long_edge_limit_;
    const size_t component_size_limit_;
    const size_t shortest_path_limit_;
    static const size_t DIJKSTRA_LIMIT = 3000;
    static const size_t BASIC_N_CNT = 100;

    bool AnyTipsInComponent(const GraphComponent<Graph>& component) const {
        for (auto e : component.edges())
            if (g_.IncomingEdgeCount(g_.EdgeStart(e)) == 0 || g_.OutgoingEdgeCount(g_.EdgeEnd(e)) == 0)
                return true;
        return false;
    }

    EdgeId FindStart(const std::set<VertexId> &component_set) const {
        EdgeId result;
        for (VertexId v : component_set) {
            for (EdgeId e : g_.IncomingEdges(v)) {
                if (component_set.count(g_.EdgeStart(e)) == 0) {
                    if (result != EdgeId())
                        return EdgeId();

                    result = e;
                }
            }
        }
        return result;
    }

    EdgeId FindFinish(const std::set<VertexId> &component_set) {
        EdgeId result;
        for (VertexId v : component_set) {
            for (EdgeId e : g_.OutgoingEdges(v)) {
                if (component_set.count(g_.EdgeEnd(e)) == 0) {
                    if (result != EdgeId())
                        return EdgeId();

                    result = e;
                }
            }
        }
        return result;
    }

    bool IsEndInsideComponent(const BidirectionalPath &path, const std::set<VertexId> &component_set) {
        if (component_set.count(g_.EdgeStart(path.Front())) == 0)
            return false;

        for (size_t i = 0; i < path.Size(); ++i)
            if (component_set.count(g_.EdgeEnd(path.At(i))) == 0)
                return false;

        return true;
    }

    bool IsEndInsideComponent(const std::vector<EdgeId> &path, const std::set<VertexId> &component_set) {
        if (component_set.count(g_.EdgeStart(path.front())) == 0)
            return false;

        for (size_t i = 0; i < path.size(); ++i)
            if (component_set.count(g_.EdgeEnd(path[i])) == 0)
                return false;

        return true;
    }

    bool IsEndInsideComponent(const BidirectionalPath &path, EdgeId component_entrance,
                              const std::set<VertexId> &component_set, bool conjugate = false) {
        int i = path.FindLast(component_entrance);
        VERIFY_MSG(i != -1, "Component edge is not found in the path")

        if ((size_t) i == path.Size() - 1) {
            if (conjugate)
                return component_set.count(g_.conjugate(g_.EdgeEnd(path.Back()))) > 0;
            return component_set.count(g_.EdgeEnd(path.Back())) > 0;
        }

        if (conjugate)
            return IsEndInsideComponent(path.SubPath((size_t) i + 1).Conjugate(), component_set);
        return IsEndInsideComponent(path.SubPath((size_t) i + 1), component_set);
    }

    bool TraverseLoop(EdgeId start, EdgeId end, const std::set<VertexId> &component_set) {
        DEBUG("start " << g_.int_id(start) << " end " << g_.int_id(end));
        BidirectionalPathSet start_cover_paths = cov_map_.GetCoveringPaths(start);
        BidirectionalPathSet end_cover_paths = cov_map_.GetCoveringPaths(end);

        for (auto path_ptr : start_cover_paths)
            if (path_ptr->FindAll(end).size() > 0)
                return false;

        if (start_cover_paths.size() < 1 || end_cover_paths.size() < 1) {
            DEBUG("TraverseLoop STRANGE SITUATION: start " << start_cover_paths.size() << " end " << end_cover_paths.size());
            return false;
        }

        if (start_cover_paths.size() > 1 || end_cover_paths.size() > 1) {
            DEBUG("Ambiguous situation in path joining, quitting");
            return false;
        }

        BidirectionalPath& start_path = **start_cover_paths.begin();
        BidirectionalPath& end_path = **end_cover_paths.begin();

        //TODO isn't it enough to check pointer equality?
        if (start_path == end_path.Conjugate()){
            return false;
        }

        //Checking that paths ends are within component
        if (!IsEndInsideComponent(start_path, start, component_set) ||
            !IsEndInsideComponent(*end_path.GetConjPath(), g_.conjugate(end), component_set, true)) {
            DEBUG("Some path goes outside of the component")
            return false;
        }

        size_t common_size = CommonEndSize(start_path, end_path);
        DEBUG("Str " << start_path.Size() << ", end" << end_path.Size());
        if (common_size == 0 && !start_path.Empty() && !end_path.Empty()) {
            DEBUG("Estimating gap size");
            VertexId last_vertex = g_.EdgeEnd(start_path.Back());
            VertexId first_vertex = g_.EdgeStart(end_path.Front());

            if (first_vertex != last_vertex) {
                auto dijkstra = DijkstraHelper<Graph>::CreateBoundedDijkstra(g_, shortest_path_limit_,
                                                                             DIJKSTRA_LIMIT,
                                                                             true /* collect traceback */);
                dijkstra.Run(last_vertex);
                auto shortest_path = dijkstra.GetShortestPathTo(first_vertex);

                if (shortest_path.empty()) {
                    DEBUG("Failed to find closing path");
                    return false;
                } 
                if (!IsEndInsideComponent(shortest_path, component_set)) {
                    DEBUG("Closing path is outside the component");
                    return false;
                }
            }
        }
        start_path.PushBack(end_path.SubPath(common_size), Gap(int(g_.k() + BASIC_N_CNT)));

        DEBUG("travers");
        start_path.PrintDEBUG();
        end_path.PrintDEBUG();
        DEBUG("conj");
        end_path.GetConjPath()->PrintDEBUG();
        end_path.Clear();
        return true;
    }

    bool ContainsLongEdges(const GraphComponent<Graph>& component) const {
        for (auto e : component.edges())
            if (g_.length(e) > long_edge_limit_)
                return true;
        return false;
    }

    size_t CommonEndSize(const SimpleBidirectionalPath& start_path, const SimpleBidirectionalPath& end_path) const {
        if (end_path.Empty())
            return 0;

        std::vector<size_t> begins = start_path.FindAll(end_path.Front());
        for (size_t i = 0; i < begins.size(); ++i) {
            size_t it1 = begins[i];
            size_t it2 = 0;
            while (it2 < end_path.Size() && start_path.At(it1) == end_path.At(it2)) {
                ++it1;
                ++it2;
                if (it1 == start_path.Size())
                    return it2;
            }
        }
        return 0;
    }

public:
    LoopTraverser(const Graph& g, GraphCoverageMap& coverage_map,
                  size_t long_edge_limit, size_t component_size_limit,
                  size_t shortest_path_limit) :
            g_(g), cov_map_(coverage_map),
            long_edge_limit_(long_edge_limit),
            component_size_limit_(component_size_limit),
            shortest_path_limit_(shortest_path_limit) {
    }

    size_t TraverseAllLoops() {
        DEBUG("TraverseAllLoops");
        size_t traversed = 0;
        auto splitter = LongEdgesExclusiveSplitter<Graph>(g_, long_edge_limit_);
        while (splitter->HasNext()) {
            GraphComponent<Graph> component = splitter->Next();
            if (component.v_size() > component_size_limit_)
                continue;
            if (ContainsLongEdges(component))
                continue;
            if (AnyTipsInComponent(component))
                continue;

            std::set<VertexId> component_set(component.v_begin(), component.v_end());
            EdgeId start = FindStart(component_set);
            EdgeId finish = FindFinish(component_set);
            if (start == EdgeId() || finish == EdgeId()) {
                continue;
            }
            if (TraverseLoop(start, finish, component_set))
                ++traversed;
        }
        return traversed;
    }

protected:
    DECL_LOGGER("LoopTraverser");
};

}

#endif /* LOOP_TRAVERSER_H_ */
