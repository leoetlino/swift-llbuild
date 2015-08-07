//===-- BuildSystem.cpp ---------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/BuildSystem.h"
#include "llbuild/BuildSystem/BuildSystemCommandInterface.h"

#include "llvm/ADT/StringRef.h"

#include "llbuild/Basic/FileInfo.h"
#include "llbuild/Core/BuildDB.h"
#include "llbuild/Core/BuildEngine.h"
#include "llbuild/BuildSystem/BuildExecutionQueue.h"
#include "llbuild/BuildSystem/BuildFile.h"
#include "llbuild/BuildSystem/BuildKey.h"
#include "llbuild/BuildSystem/BuildValue.h"

#include <memory>

using namespace llbuild;
using namespace llbuild::basic;
using namespace llbuild::core;
using namespace llbuild::buildsystem;

BuildExecutionQueue::~BuildExecutionQueue() {}

BuildSystemDelegate::~BuildSystemDelegate() {}

BuildSystemCommandInterface::~BuildSystemCommandInterface() {}

#pragma mark - BuildSystem implementation

namespace {

class BuildSystemImpl;

/// The delegate used to load the build file for use by a build system.
class BuildSystemFileDelegate : public BuildFileDelegate {
  BuildSystemImpl& system;

public:
  BuildSystemFileDelegate(BuildSystemImpl& system)
      : BuildFileDelegate(), system(system) {}

  BuildSystemDelegate& getSystemDelegate();

  /// @name Delegate Implementation
  /// @{

  virtual void error(const std::string& filename,
                     const std::string& message) override;

  virtual bool configureClient(const std::string& name,
                               uint32_t version,
                               const property_list_type& properties) override;

  virtual std::unique_ptr<Tool> lookupTool(const std::string& name) override;

  virtual void loadedTarget(const std::string& name,
                            const Target& target) override;

  virtual void loadedCommand(const std::string& name,
                             const Command& target) override;

  virtual std::unique_ptr<Node> lookupNode(const std::string& name,
                                           bool isImplicit=false) override;

  /// @}
};

/// The delegate used to build a loaded build file.
class BuildSystemEngineDelegate : public BuildEngineDelegate {
  BuildSystemImpl& system;

  BuildFile& getBuildFile();

  virtual Rule lookupRule(const KeyType& keyData) override;
  virtual void cycleDetected(const std::vector<Rule*>& items) override;

public:
  BuildSystemEngineDelegate(BuildSystemImpl& system) : system(system) {}

  BuildSystemImpl& getBuildSystem() {
    return system;
  }
};

class BuildSystemImpl : public BuildSystemCommandInterface {
  BuildSystem& buildSystem;

  /// The delegate the BuildSystem was configured with.
  BuildSystemDelegate& delegate;

  /// The name of the main input file.
  std::string mainFilename;

  /// The delegate used for the loading the build file.
  BuildSystemFileDelegate fileDelegate;

  /// The build file the system is building.
  BuildFile buildFile;

  /// The delegate used for building the file contents.
  BuildSystemEngineDelegate engineDelegate;

  /// The build engine.
  BuildEngine buildEngine;

  /// The execution queue.
  std::unique_ptr<BuildExecutionQueue> executionQueue;

  /// @name BuildSystemCommandInterface Implementation
  /// @{

  virtual BuildExecutionQueue& getExecutionQueue() override {
    return *executionQueue;
  }

  virtual void taskNeedsInput(core::Task* task, const BuildKey& key,
                              uintptr_t inputID) override {
    return buildEngine.taskNeedsInput(task, key.toData(), inputID);
  }

  virtual void taskMustFollow(core::Task* task, const BuildKey& key) override {
    return buildEngine.taskMustFollow(task, key.toData());
  }

  virtual void taskDiscoveredDependency(core::Task* task,
                                        const BuildKey& key) override {
    return buildEngine.taskDiscoveredDependency(task, key.toData());
  }

  virtual void taskIsComplete(core::Task* task, const BuildValue& value,
                              bool forceChange) override {
    return buildEngine.taskIsComplete(task, value.toData(), forceChange);
  }

  virtual void addJob(QueueJob&& job) override {
    executionQueue->addJob(std::move(job));
  }

  /// @}

public:
  BuildSystemImpl(class BuildSystem& buildSystem,
                  BuildSystemDelegate& delegate,
                  const std::string& mainFilename)
      : buildSystem(buildSystem), delegate(delegate),
        mainFilename(mainFilename),
        fileDelegate(*this), buildFile(mainFilename, fileDelegate),
        engineDelegate(*this), buildEngine(engineDelegate),
        executionQueue(delegate.createExecutionQueue()) {}

  BuildSystem& getBuildSystem() {
    return buildSystem;
  }

  BuildSystemDelegate& getDelegate() {
    return delegate;
  }

  const std::string& getMainFilename() {
    return mainFilename;
  }

  BuildSystemCommandInterface& getCommandInterface() {
    return *this;
  }

  BuildFile& getBuildFile() {
    return buildFile;
  }

  BuildEngine& getBuildEngine() {
    return buildEngine;
  }

  /// @name Client API
  /// @{

  bool attachDB(const std::string& filename, std::string* error_out) {
    // FIXME: How do we pass the client schema version here, if we haven't
    // loaded the file yet.
    std::unique_ptr<core::BuildDB> db(
        core::createSQLiteBuildDB(filename, delegate.getVersion(),
                                  error_out));
    if (!db)
      return false;

    buildEngine.attachDB(std::move(db));
    return true;
  }

  bool enableTracing(const std::string& filename, std::string* error_out) {
    return buildEngine.enableTracing(filename, error_out);
  }

  bool build(const std::string& target);

  /// @}
};

#pragma mark - BuildSystem engine integration

#pragma mark - Task implementations

/// This is the task used to "build" a target, it translates between the request
/// for building a target key and the requests for all of its nodes.
class TargetTask : public Task {
  Target& target;

  virtual void start(BuildEngine& engine) override {
    // Request all of the necessary system tasks.
    for (const auto& nodeName: target.getNodeNames()) {
      engine.taskNeedsInput(this, BuildKey::makeNode(nodeName).toData(),
                            /*InputID=*/0);
    }
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
    // Do nothing.
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
    // Do nothing.
    //
    // FIXME: We may need to percolate an error status here.
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // Complete the task immediately.
    engine.taskIsComplete(this, ValueType());
  }

public:
  TargetTask(Target& target) : target(target) {}
};

/// This is the task to "build" a node which represents pure raw input to the
/// system.
class InputNodeTask : public Task {
  Node& node;

  virtual void start(BuildEngine& engine) override {
    assert(node.getProducers().empty());
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // Get the information on the file.
    //
    // FIXME: This needs to delegate, since we want to have a notion of
    // different node types.
    auto info = FileInfo::getInfoForPath(node.getName());
    if (info.isMissing()) {
      engine.taskIsComplete(this, BuildValue::makeMissingInput().toData());
      return;
    }

    engine.taskIsComplete(
        this, BuildValue::makeExistingInput(info).toData());
  }

public:
  InputNodeTask(Node& node) : node(node) {}

  static bool isResultValid(const Node& node, const BuildValue& value) {
    // If the previous value wasn't for an existing input, always recompute.
    if (!value.isExistingInput())
      return false;

    // Otherwise, the result is valid if the path exists and the file
    // information remains the same.
    //
    // FIXME: This is inefficient, we will end up doing the stat twice, once
    // when we check the value for up to dateness, and once when we "build" the
    // output.
    //
    // We can solve this by caching ourselves but I wonder if it is something
    // the engine should support more naturally.
    auto info = FileInfo::getInfoForPath(node.getName());
    if (info.isMissing())
      return false;

    return value.getOutputInfo() == info;
  }
};


/// This is the task to "build" a node which is the product of some command.
///
/// It is responsible for selecting the appropriate producer command to run to
/// produce the node, and for synchronizing any external state the node depends
/// on.
class ProducedNodeTask : public Task {
  Node& node;

  virtual void start(BuildEngine& engine) override {
    // Request the producer command.
    if (node.getProducers().size() == 1) {
      auto command = node.getProducers()[0];
      engine.taskNeedsInput(this,
                            BuildKey::makeCommand(command->getName()).toData(),
                            /*InputID=*/0);
      return;
    }

    // FIXME: Delegate to the client to select the appropriate producer if
    // there are more than one.
    assert(0 && "FIXME: not implemented (support for non-unary producers");
    abort();
  }

  virtual void providePriorValue(BuildEngine&,
                                 const ValueType& value) override {
  }

  virtual void provideValue(BuildEngine&, uintptr_t inputID,
                            const ValueType& value) override {
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    // Complete the task immediately.
    engine.taskIsComplete(this, ValueType());
  }

public:
  ProducedNodeTask(Node& node) : node(node) {}
};

/// This is the task to actually execute a command.
class CommandTask : public Task {
  Command& command;

  static BuildSystemImpl& getBuildSystem(BuildEngine& engine) {
    return static_cast<BuildSystemEngineDelegate*>(
        engine.getDelegate())->getBuildSystem();
  }

  virtual void start(BuildEngine& engine) override {
    command.start(getBuildSystem(engine).getCommandInterface(), this);
  }

  virtual void providePriorValue(BuildEngine& engine,
                                 const ValueType& valueData) override {
    BuildValue value = BuildValue::fromData(valueData);
    command.providePriorValue(
        getBuildSystem(engine).getCommandInterface(), this, value);
  }

  virtual void provideValue(BuildEngine& engine, uintptr_t inputID,
                            const ValueType& valueData) override {
    command.provideValue(
        getBuildSystem(engine).getCommandInterface(), this, inputID,
        BuildValue::fromData(valueData));
  }

  virtual void inputsAvailable(BuildEngine& engine) override {
    command.inputsAvailable(getBuildSystem(engine).getCommandInterface(), this);
  }

public:
  CommandTask(Command& command) : command(command) {}

  static bool isResultValid(Command& command, const BuildValue& value) {
    return command.isResultValid(value);
  }
};

#pragma mark - BuildSystemEngineDelegate implementation

BuildFile& BuildSystemEngineDelegate::getBuildFile() {
  return system.getBuildFile();
}

Rule BuildSystemEngineDelegate::lookupRule(const KeyType& keyData) {
  // Decode the key.
  auto key = BuildKey::fromData(keyData);

  switch (key.getKind()) {
  default:
    assert(0 && "invalid key");
    abort();

  case BuildKey::Kind::Command: {
    // Find the comand.
    auto it = getBuildFile().getCommands().find(key.getCommandName());
    if (it == getBuildFile().getCommands().end()) {
      assert(0 && "unexpected request for missing command");
      abort();
    }

    // Create the rule for the command.
    Command* command = it->second.get();
    return Rule{
      keyData,
      /*Action=*/ [command](BuildEngine& engine) -> Task* {
        return engine.registerTask(new CommandTask(*command));
      },
      /*IsValid=*/ [command](const Rule& rule, const ValueType& value) -> bool {
        return CommandTask::isResultValid(
            *command, BuildValue::fromData(value));
      }
    };
  }

  case BuildKey::Kind::Node: {
    // Find the node.
    auto it = getBuildFile().getNodes().find(key.getNodeName());
    if (it == getBuildFile().getNodes().end()) {
      // FIXME: Unknown node name, should map to a default type (a file
      // generally, although we might want to provide a way to put this under
      // control of the client).
      assert(0 && "FIXME: unknown node");
      abort();
    }

    // Create the rule used to construct this node.
    //
    // We could bypass this level and directly return the rule to run the
    // command, which would reduce the number of tasks in the system. For now we
    // do the uniform thing, but do differentiate between input and command
    // nodes.
    Node* node = it->second.get();

    // Create an input node if there are no producers.
    if (node->getProducers().empty()) {
      return Rule{
        keyData,
        /*Action=*/ [node](BuildEngine& engine) -> Task* {
          return engine.registerTask(new InputNodeTask(*node));
        },
        /*IsValid=*/ [node](const Rule& rule, const ValueType& value) -> bool {
          return InputNodeTask::isResultValid(
              *node, BuildValue::fromData(value));
        }
      };
    }

    // Otherwise, create a task for a produced node.
    return Rule{
      keyData,
      /*Action=*/ [node](BuildEngine& engine) -> Task* {
        return engine.registerTask(new ProducedNodeTask(*node));
      }
      // FIXME: Check node validity.
    };
  }

  case BuildKey::Kind::Target: {
    // Find the target.
    auto it = getBuildFile().getTargets().find(key.getTargetName());
    if (it == getBuildFile().getTargets().end()) {
      // FIXME: Invalid target name, produce an error.
      assert(0 && "FIXME: invalid target");
      abort();
    }

    // Create the rule to construct this target.
    Target* target = it->second.get();
    return Rule{
      keyData,
        /*Action=*/ [target](BuildEngine& engine) -> Task* {
        return engine.registerTask(new TargetTask(*target));
      }
      // FIXME: Check target validitity, we need to check the target list hasn't
      // changed.
    };
  }
  }
}

void BuildSystemEngineDelegate::cycleDetected(const std::vector<Rule*>& items) {
  system.getDelegate().error(system.getMainFilename(),
                             "cycle detected while building");
}

#pragma mark - BuildSystemImpl implementation

bool BuildSystemImpl::build(const std::string& target) {
  // Load the build file.
  //
  // FIXME: Eventually, we may want to support something fancier where we load
  // the build file in the background so we can immediately start building
  // things as they show up.
  //
  // FIXME: We need to load this only once.
  getBuildFile().load();

  // Build the target.
  getBuildEngine().build(BuildKey::makeTarget(target).toData());

  return false;
}

#pragma mark - BuildNode implementation

// FIXME: Figure out how this is going to be organized.
class BuildNode : public Node {
public:
  using Node::Node;

  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    // We don't support any custom attributes.
    return false;
  }
};

#pragma mark - ShellTool implementation

class ShellCommand : public Command {
  BuildSystemImpl& system;
  std::vector<Node*> inputs;
  std::vector<Node*> outputs;
  std::string args;

public:
  ShellCommand(BuildSystemImpl& system, const std::string& name)
      : Command(name), system(system) {}

  virtual void configureInputs(const std::vector<Node*>& value) override {
    inputs = value;
  }

  virtual void configureOutputs(const std::vector<Node*>& value) override {
    outputs = value;
  }

  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    if (name == "args") {
      args = value;
    } else {
      system.getDelegate().error(
          system.getMainFilename(),
          "unexpected attribute: '" + name + "'");
      return false;
    }

    return true;
  }

  virtual bool isResultValid(const BuildValue& value) override {
    // If the previous value wasn't for a successful command, always recompute.
    if (!value.isSuccessfulCommand())
      return false;

    // FIXME: Check command signature.

    // Check the timestamps on each of the outputs.
    for (unsigned i = 0, e = outputs.size(); i != e; ++i) {
      auto* node = outputs[i];

      // Always rebuild if the output is missing.
      auto info = FileInfo::getInfoForPath(node->getName());
      if (info.isMissing())
        return false;

      // Otherwise, the result is valid if the file information has not changed.
      if (value.getNthOutputInfo(i) != info)
        return false;
    }

    // Otherwise, the result is ok.
    return true;
  }

  virtual void start(BuildSystemCommandInterface& system, Task* task) override {
    // Request all of the inputs.
    for (const auto& node: inputs) {
      system.taskNeedsInput(task, BuildKey::makeNode(node), /*InputID=*/0);
    }
  }

  virtual void providePriorValue(BuildSystemCommandInterface&, Task*,
                                 const BuildValue&) override {
  }

  virtual void provideValue(BuildSystemCommandInterface&, Task*,
                            uintptr_t inputID,
                            const BuildValue& value) override {
  }

  virtual void inputsAvailable(BuildSystemCommandInterface& system,
                               Task* task) override {
    // Suppress static analyzer false positive on generalized lambda capture
    // (rdar://problem/22165130).
#ifndef __clang_analyzer__
    auto fn = [this, &system=system, task](QueueJobContext* context) {
      // Log the command.
      //
      // FIXME: Design the logging and status output APIs.
      fprintf(stdout, "%s\n", args.c_str());

      // Execute the command.
      if (!system.getExecutionQueue().executeShellCommand(context, args)) {
        // If the command failed, the result is failure.
        system.taskIsComplete(task, BuildValue::makeFailedCommand());
        return;
      }

      // Capture the file information for each of the output nodes.
      //
      // FIXME: We need to delegate to the node here.
      llvm::SmallVector<FileInfo, 8> outputInfos;
      for (auto* node: outputs) {
        outputInfos.push_back(FileInfo::getInfoForPath(node->getName()));
      }
      
      // Otherwise, complete with a successful result.
      system.taskIsComplete(
          task, BuildValue::makeSuccessfulCommand(outputInfos));
    };
    system.addJob({ this, std::move(fn) });
#endif
  }
};

class ShellTool : public Tool {
  BuildSystemImpl& system;

public:
  ShellTool(BuildSystemImpl& system, const std::string& name)
      : Tool(name), system(system) {}

  virtual bool configureAttribute(const std::string& name,
                                  const std::string& value) override {
    system.getDelegate().error(
        system.getMainFilename(),
        "unexpected attribute: '" + name + "'");

    // No supported attributes.
    return false;
  }

  virtual std::unique_ptr<Command> createCommand(
      const std::string& name) override {
    return std::make_unique<ShellCommand>(system, name);
  }
};

#pragma mark - BuildSystemFileDelegate

BuildSystemDelegate& BuildSystemFileDelegate::getSystemDelegate() {
  return system.getDelegate();
}

void BuildSystemFileDelegate::error(const std::string& filename,
                                    const std::string& message) {
  // Delegate to the system delegate.
  getSystemDelegate().error(filename, message);
}

bool
BuildSystemFileDelegate::configureClient(const std::string& name,
                                         uint32_t version,
                                         const property_list_type& properties) {
  // The client must match the configured name of the build system.
  if (name != getSystemDelegate().getName())
    return false;

  // The client version must match the configured version.
  //
  // FIXME: We should give the client the opportunity to support a previous
  // schema version (auto-upgrade).
  if (version != getSystemDelegate().getVersion())
    return false;

  return true;
}

std::unique_ptr<Tool>
BuildSystemFileDelegate::lookupTool(const std::string& name) {
  // First, give the client an opportunity to create the tool.
  auto tool = getSystemDelegate().lookupTool(name);
  if (tool)
    return std::move(tool);

  // Otherwise, look for one of the builtin tool definitions.
  if (name == "shell") {
    return std::make_unique<ShellTool>(system, name);
  }

  return nullptr;
}

void BuildSystemFileDelegate::loadedTarget(const std::string& name,
                                           const Target& target) {
}

void BuildSystemFileDelegate::loadedCommand(const std::string& name,
                                            const Command& command) {
}

std::unique_ptr<Node>
BuildSystemFileDelegate::lookupNode(const std::string& name,
                                    bool isImplicit) {
  return std::make_unique<BuildNode>(name);
}

}

#pragma mark - BuildSystem

BuildSystem::BuildSystem(BuildSystemDelegate& delegate,
                         const std::string& mainFilename)
    : impl(new BuildSystemImpl(*this, delegate, mainFilename))
{
}

BuildSystem::~BuildSystem() {
  delete static_cast<BuildSystemImpl*>(impl);
}

BuildSystemDelegate& BuildSystem::getDelegate() {
  return static_cast<BuildSystemImpl*>(impl)->getDelegate();
}

bool BuildSystem::attachDB(const std::string& path,
                                std::string* error_out) {
  return static_cast<BuildSystemImpl*>(impl)->attachDB(path, error_out);
}

bool BuildSystem::enableTracing(const std::string& path,
                                std::string* error_out) {
  return static_cast<BuildSystemImpl*>(impl)->enableTracing(path, error_out);
}

bool BuildSystem::build(const std::string& name) {
  return static_cast<BuildSystemImpl*>(impl)->build(name);
}