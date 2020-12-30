#pragma once

#include "cuda_graph.hpp"

/** 
@file cuda_optimizer.hpp
@brief %cudaFlow capturer optimization algorithms include file
*/

namespace tf {

// ----------------------------------------------------------------------------
// OptimizerBase
// ----------------------------------------------------------------------------

/**
@private

@brief class to provide helper common methods for optimization algorithms
*/
class OptimizerBase {

  protected:

    std::vector<cudaNode*> _toposort(cudaGraph&);
    std::vector<std::vector<cudaNode*>> _levelize(cudaGraph&);
};

// Function: _toposort
inline std::vector<cudaNode*> OptimizerBase::_toposort(cudaGraph& graph) {

  std::stack<cudaNode*> dfs;
  std::vector<cudaNode*> res;

  for(auto node : graph._nodes) {
    node->_unset_state(cudaNode::STATE_VISITED);
  }

  for(auto node : graph._nodes) {
    if(!node->_has_state(cudaNode::STATE_VISITED)) {
      dfs.push(node);
    }

    while(!dfs.empty()) {
      auto u = dfs.top();
      dfs.pop();

      if(u->_has_state(cudaNode::STATE_VISITED)){
        res.push_back(u);
        continue;
      }

      u->_set_state(cudaNode::STATE_VISITED);
      dfs.push(u);

      for(auto s : u->_successors) {
        if(!(s->_has_state(cudaNode::STATE_VISITED))) {
          dfs.push(s);
        }
      }
    }
  }

  std::reverse(res.begin(), res.end());
  
  return res;
}

// Function: _levelize
inline std::vector<std::vector<cudaNode*>> 
OptimizerBase::_levelize(cudaGraph& graph) {

  std::queue<cudaNode*> bfs;

  size_t max_level = 0;
  
  // insert the first level of nodes into the queue
  for(auto node : graph._nodes) {
    node->_unset_state(cudaNode::STATE_VISITED);
    if(node->_dependents.size() == 0) {
      bfs.push(node);
      std::get<cudaNode::Capture>(node->_handle).level = 0;
    }
  }
  
  // levelize the graph using bfs
  while(!bfs.empty()) {

    auto u = bfs.front();
    bfs.pop();
    
    auto& hu = std::get<cudaNode::Capture>(u->_handle);

    for(auto v : u->_successors) {
      if(!(v->_has_state(cudaNode::STATE_VISITED))) {
        auto& hv = std::get<cudaNode::Capture>(v->_handle);
        hv.level = hu.level + 1;
        if(hv.level > max_level) {
          max_level = hv.level;
        }
        bfs.push(v);
      }
    }
  }
  
  // YOUR BFS IS WRONG ... You need to start with the first level
  // that doesn't have any precedessors.
  // Consider a simple example that fails your implementation:
  // nodes = {B, A, C}, dependencies: A->B->C
  // You start with B and set its level to 0 and C to 1. 
  // Next, you start with A and set its level to 0. 
  //
  //for(auto node: nodes) {
  //  
  //  auto& cur_node_level = std::get<cudaNode::Capture>(node->_handle).level;

  //  if(!(node->_has_state(cudaNode::STATE_VISITED))) {
  //    node->_set_state(cudaNode::STATE_VISITED);
  //    bfs.push(node);
  //    cur_node_level = 0;
  //  }

  //  while(!bfs.empty()) {
  //    auto u = bfs.front();
  //    bfs.pop();
  //    for(auto s: u->_successors) {

  //      auto& suc_node_level = std::get<cudaNode::Capture>(s->_handle).level;

  //      suc_node_level = cur_node_level + 1;

  //      if(!(s->_has_state(cudaNode::STATE_VISITED))) {
  //        s->_set_state(cudaNode::STATE_VISITED);
  //        bfs.push(s);
  //      }
  //    }
  //  }

  //  _max_level = std::max(_max_level, cur_node_level);
  //} 

  //set level_graph and each node's idx
  std::vector<std::vector<cudaNode*>> level_graph(max_level+1);
  for(auto u : graph._nodes) {
    auto& hu = std::get<cudaNode::Capture>(u->_handle);
    hu.idx = level_graph[hu.level].size();
    level_graph[hu.level].emplace_back(u);
  }
  
  return level_graph;
}

// ----------------------------------------------------------------------------
// class definition: SequentialOptimizer
// ----------------------------------------------------------------------------

/**
@class SequentialOptimizer

@brief class to capture the described graph into a native cudaGraph
       using a single stream

A sequential optimizer finds a topological order of the described graph and
captures dependent GPU tasks using a single stream.
All GPU tasks run sequentially without breaking inter dependencies.
*/
class SequentialOptimizer : public OptimizerBase {

  friend class cudaFlowCapturer;

  public:
    
    /**
    @brief constructs a sequential optimizer
    */
    SequentialOptimizer() = default;
  
  private:

    cudaGraph_t _optimize(cudaGraph& graph);
};

inline cudaGraph_t SequentialOptimizer::_optimize(cudaGraph& graph) {
  // acquire per-thread stream and turn it into capture mode
  // we must use ThreadLocal mode to avoid clashing with CUDA global states
  cudaScopedPerThreadStream stream;

  cudaGraph_t native_g;

  TF_CHECK_CUDA(
    cudaStreamBeginCapture(stream, cudaStreamCaptureModeThreadLocal), 
    "failed to turn stream into per-thread capture mode"
  );

  auto ordered = _toposort(graph);
  for(auto& node : ordered) {
    std::get<cudaNode::Capture>(node->_handle).work(stream);  
  }

  TF_CHECK_CUDA(
    cudaStreamEndCapture(stream, &native_g), "failed to end capture"
  );

  return native_g;
}

// ----------------------------------------------------------------------------
// class definition: RoundRobinOptimizer
// ----------------------------------------------------------------------------

/**
@class RoundRobinOptimizer

@brief class to capture the described graph into a native cudaGraph
       using a greedy round-robin algorithm on a fixed number of streams
*/
class RoundRobinOptimizer : public OptimizerBase {

  friend class cudaFlowCapturer;

  public:
    
    /**
    @brief constructs a round-robin optimizer with 4 streams by default
     */
    RoundRobinOptimizer();
    
    /**
    @brief constructs a round-robin optimizer with the given number of streams
     */
    RoundRobinOptimizer(size_t num_streams);
    
    /**
    @brief queries the number of streams used by the optimizer
     */
    size_t num_streams() const;

    /**
    @brief sets the number of streams used by the optimizer
     */
    void num_streams(size_t n);

  private:

    size_t _num_streams {4};

    cudaGraph_t _optimize(cudaGraph& graph);
};

// Constructor
inline RoundRobinOptimizer::RoundRobinOptimizer(size_t num_streams) :
  _num_streams {num_streams} {

  if(num_streams == 0) {
    TF_THROW("number of streams must be at least one");
  }
}

// Function: num_streams
inline size_t RoundRobinOptimizer::num_streams() const {
  return _num_streams;
}

// Procedure: num_streams
inline void RoundRobinOptimizer::num_streams(size_t n) {
  if(n == 0) {
    TF_THROW("number of streams must be at least one");
  }
  _num_streams = n;
}

// Function: _optimize 
inline cudaGraph_t RoundRobinOptimizer::_optimize(cudaGraph& graph) {
  
  // levelize the graph
  auto level_graph = _levelize(graph);
  
  // begin to capture
  std::vector<cudaScopedPerThreadStream> streams(_num_streams);

  TF_CHECK_CUDA(
    cudaStreamBeginCapture(streams[0], cudaStreamCaptureModeThreadLocal), 
    "failed to turn stream into per-thread capture mode"
  );
  
  // reserve space for scoped events
  std::vector<cudaScopedPerThreadEvent> events;
  events.reserve((_num_streams >> 1) + level_graph.size());
  
  // fork
  cudaEvent_t fork_event = events.emplace_back();
  TF_CHECK_CUDA(
    cudaEventRecord(fork_event, streams[0]), "faid to record fork"
  );

  for(size_t i = 1; i < streams.size(); ++i) {
    TF_CHECK_CUDA(
      cudaStreamWaitEvent(streams[i], fork_event, 0), "failed to wait on fork"
    );
  }

  // assign streams to levelized nodes in a round-robin manner
  for(auto& each_level_nodes : level_graph) {
    for(size_t i = 0; i < each_level_nodes.size(); ++i) {

      auto& node  = each_level_nodes[i];
      auto& succs = node->_successors;
      auto& preds = node->_dependents;
      auto& hn = std::get<cudaNode::Capture>(node->_handle);
      
      // stream id assigned to this node
      size_t sid = i % _num_streams;

      //wait event in previous level
      for(size_t p = 0; p < preds.size(); ++p) {
        auto& hp = std::get<cudaNode::Capture>(preds[p]->_handle);

        if(hp.idx % _num_streams != sid) {
          TF_CHECK_CUDA(
            cudaStreamWaitEvent(streams[sid], hp.event),
            "failed to wait on predecessor"
          )
        }

        //if((std::get<cudaNode::Capture>(preds[p]->_handle).idx % _num_streams) != sid) {
        //  TF_CHECK_CUDA(cudaStreamWaitEvent(streams[sid], std::get<cudaNode::Capture>(preds[p]->_handle).event, 0), "failed to wait event");
        //}
      }
      
      // enqueu the work
      hn.work(streams[sid]);
      //std::get<cudaNode::Capture>(node->_handle).work(streams[sid]);  

      //create event if there is a node in the next level executed in different stream
      for(size_t k = 0; k < succs.size(); ++k) {
        auto& hs = std::get<cudaNode::Capture>(succs[k]->_handle);
        if(hs.idx % _num_streams != sid) {
          hn.event = events.emplace_back();
          TF_CHECK_CUDA(
            cudaEventRecord(hn.event, streams[sid]), "failed to record event"
          );
        }
        //if((std::get<cudaNode::Capture>(succs[k]->_handle).idx % _num_streams) != sid) {
        //  TF_CHECK_CUDA(cudaEventCreate(&(std::get<cudaNode::Capture>(node->_handle).event)), "failed to create event");
        //  TF_CHECK_CUDA(cudaEventRecord(std::get<cudaNode::Capture>(node->_handle).event, streams[sid]), "failed to record event");
        //  break;
        //}
      }
    }
      
  }

  // join
  for(size_t i=1; i<_num_streams; ++i) {
    cudaEvent_t join_event = events.emplace_back();
    TF_CHECK_CUDA(
      cudaEventRecord(join_event, streams[i]), "failed to record join"
    );
    TF_CHECK_CUDA(
      cudaStreamWaitEvent(streams[0], join_event), "failed to wait on join"
    );
  }

  //std::vector<cudaEvent_t> join_events{_num_streams - 1};
  //for(size_t i = 0; i < join_events.size(); ++i) {
  //  TF_CHECK_CUDA(cudaEventCreate(&join_events[i]), "failed to create event");
  //  TF_CHECK_CUDA(cudaEventRecord(join_events[i], streams[i+1]), "failed to record event");
  //  TF_CHECK_CUDA(cudaStreamWaitEvent(streams[0], join_events[i], 0), "failed to wait event");
  //}

  cudaGraph_t native_g;

  TF_CHECK_CUDA(
    cudaStreamEndCapture(streams[0], &native_g), "failed to end capture"
  );

  return native_g;
}

}  // end of namespace tf -----------------------------------------------------
