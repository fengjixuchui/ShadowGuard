#ifndef LITECFI_CALL_GRAPH_IMPL_H_
#define LITECFI_CALL_GRAPH_IMPL_H_

#include <algorithm>
#include <string>

#include "BPatch.h"
#include "BPatch_function.h"
#include "BPatch_object.h"
#include "CodeObject.h"
#include "glog/logging.h"
#include "parse.h"

// ------------------ LazyFunction ------------------------ //

template <typename T>
std::map<Dyninst::ParseAPI::Function*, LazyFunction<T>*>
    LazyFunction<T>::lazy_func_map;

template <typename T>
LazyFunction<T>* LazyFunction<T>::GetInstance(
    Dyninst::ParseAPI::Function* const function) {
  auto it = lazy_func_map.find(function);
  if (it != lazy_func_map.end()) {
    return it->second;
  }

  LazyFunction<T>* lazy_function = new LazyFunction<T>();
  lazy_function->name = function->name();
  lazy_function->function = function;

  lazy_func_map.insert(
      std::pair<Dyninst::ParseAPI::Function*, LazyFunction<T>*>(function,
                                                                lazy_function));
  return lazy_function;
}

template <typename T>
std::set<LazyFunction<T>*> LazyFunction<T>::GetCallees() {
  if (initialized) {
    // Callees already initialized
    return callees;
  }

  // Find the call edges from this function
  const Dyninst::ParseAPI::Function::edgelist& call_edges =
      function->callEdges();

  // Iterate and find call edge targets
  for (auto const& edge : call_edges) {
    // Only consider inter procedural edges
    if (edge->interproc()) {
      Dyninst::ParseAPI::Block* block = edge->trg();

      if (!block || block == reinterpret_cast<Dyninst::ParseAPI::Block*>(-1)) {
        unknown_callees = true;
        continue;
      }

      std::vector<Dyninst::ParseAPI::Function*> functions;
      // Get the target functions of this call edge
      block->getFuncs(functions);

      for (auto func : functions) {
        LazyFunction<T>* lazy_function;

        // We lookup using the real function definition which we can get via
        // LazyCallGraph::GetFunctionDefinition in case the current function
        // refers to a plt stub corresponding to the actual function.
        auto map_iter = lazy_func_map.find(
            LazyCallGraph<T>::GetFunctionDefinition(func->name()));

        // There should be a LazyFunction entry for this function since lazy
        // call graph initial processing should have happened by this stage
        DCHECK(map_iter != lazy_func_map.end())
            << "Function " << func->name() << " undefined.";

        lazy_function = map_iter->second;
        callees.insert(lazy_function);
      }
    }
  }

  initialized = true;
  return callees;
}

// -------------------- LazyCallGraph --------------------- //

template <typename T>
std::map<std::string, LazyFunction<T>*> LazyCallGraph<T>::functions_;

template <typename T>
std::map<std::string, Dyninst::ParseAPI::Function*>
    LazyCallGraph<T>::function_defs_;

template <typename T>
Dyninst::ParseAPI::Function* LazyCallGraph<T>::GetFunctionDefinition(
    std::string name) {
  auto it = function_defs_.find(name);
  /*
  DCHECK(it != function_defs_.end())
      << "No definition of the function : " << name;
      */
  return it->second;
}

template <typename T>
bool LazyCallGraph<T>::IsPltFunction(
    Dyninst::ParseAPI::Function* function,
    const std::map<Dyninst::Address, std::string>& linkage) {
  return (linkage.find(function->addr()) != linkage.end());
}

template <typename T>
void LazyCallGraph<T>::PopulateFunctionDefinitions(
    const std::vector<Dyninst::ParseAPI::CodeObject*>& code_objects) {
  for (auto code_object : code_objects) {
    std::map<Dyninst::Address, std::string> linkage =
        code_object->cs()->linkage();

    for (auto function : code_object->funcs()) {
      function_defs_.insert(
          std::pair<std::string, Dyninst::ParseAPI::Function*>(function->name(),
                                                               function));
    }
  }
}

template <typename T>
LazyCallGraph<T>* LazyCallGraph<T>::GetCallGraph(const Parser& parser) {
  std::vector<BPatch_object*> objects;
  parser.image->getObjects(objects);

  // Convert to code objects
  std::vector<Dyninst::ParseAPI::CodeObject*> code_objects;
  std::map<std::string, Dyninst::ParseAPI::CodeObject*> code_object_map;
  for (auto object : objects) {
    Dyninst::ParseAPI::CodeObject* code_object =
        Dyninst::ParseAPI::convert(object);
    code_objects.push_back(code_object);
    code_object_map.insert(
        std::pair<std::string, Dyninst::ParseAPI::CodeObject*>(
            object->pathName(), code_object));
  }

  // Resolve function definitions for all functions
  PopulateFunctionDefinitions(code_objects);

  // Construct the lazy call graph
  LazyCallGraph<T>::functions_.clear();
  LazyCallGraph<T>* cg = new LazyCallGraph<T>();

  for (auto const& it : code_object_map) {
    Dyninst::ParseAPI::CodeObject* code_object = it.second;
    for (auto function : code_object->funcs()) {
      Dyninst::ParseAPI::Function* func =
          GetFunctionDefinition(function->name());
      if (func) {
        cg->functions_.insert(std::pair<std::string, LazyFunction<T>*>(
            function->name(), LazyFunction<T>::GetInstance(func)));
      }
    }
  }
  return cg;
}

template <typename T>
LazyFunction<T>* LazyCallGraph<T>::GetFunction(std::string name) {
  auto it = functions_.find(name);
  DCHECK(it != functions_.end());
  return it->second;
}

template <typename T>
void LazyCallGraph<T>::VisitCallGraph(
    LazyFunction<T>* const function,
    std::function<void(LazyFunction<T>* const)> pre_callback,
    std::function<void(LazyFunction<T>* const)> post_callback,
    std::set<LazyFunction<T>*>* const visited) {
  if (visited->find(function) != visited->end()) {
    return;
  }

  // Update and get child info for current function
  std::set<LazyFunction<T>*> callees = function->GetCallees();

  // Run pre callback on current node
  pre_callback(function);

  visited->insert(function);

  // Visit children
  for (LazyFunction<T>* callee : callees) {
    VisitCallGraph(callee, pre_callback, post_callback, visited);
  }

  // Run post callback on current node
  if (post_callback) {
    post_callback(function);
  }
}

template <typename T>
T* LazyCallGraph<T>::VisitCallGraph(
    std::string root, std::function<void(LazyFunction<T>* const)> pre_callback,
    std::function<void(LazyFunction<T>* const)> post_callback) {
  std::set<LazyFunction<T>*> visited;
  LazyFunction<T>* function = GetFunction(root);

  VisitCallGraph(function, pre_callback, post_callback, &visited);

  return function->data;
}

#endif  // LITECFI_CALL_GRAPH_IMPL_H_