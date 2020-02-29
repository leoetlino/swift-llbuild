//===-- BuildFile.cpp -----------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "llbuild/BuildSystem/BuildFile.h"

#include "llbuild/Basic/FileSystem.h"
#include "llbuild/Basic/LLVM.h"
#include "llbuild/BuildSystem/BuildDescription.h"
#include "llbuild/BuildSystem/BuildFileFormat.h"
#include "llbuild/BuildSystem/Command.h"
#include "llbuild/BuildSystem/Tool.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"

using namespace llbuild;
using namespace llbuild::buildsystem;

void ConfigureContext::error(const Twine& message) const {
  delegate.error(filename, at, message);
}

BuildFileDelegate::~BuildFileDelegate() {}

#pragma mark - BuildFile implementation

namespace {

class BuildFileImpl {
  /// The name of the main input file.
  std::string mainFilename;

  /// The delegate the BuildFile was configured with.
  BuildFileDelegate& delegate;

  /// The set of all registered tools.
  BuildDescription::tool_set tools;

  /// The set of all declared targets.
  BuildDescription::target_set targets;

  /// Default target name
  std::string defaultTarget;

  /// The set of all declared nodes.
  BuildDescription::node_set nodes;

  /// The set of all declared commands.
  BuildDescription::command_set commands;

  /// The number of parsing errors.
  int numErrors = 0;

  /// Emit an error.
  void error(StringRef filename, llvm::SMRange at, StringRef message) {
    BuildFileToken atToken{
        at.Start.getPointer(),
        unsigned(at.End.getPointer() - at.Start.getPointer())};
    delegate.error(mainFilename, atToken, message);
    ++numErrors;
  }

  void error(const Twine& message) {
    SmallString<256> buffer;
    error(mainFilename, {}, message.toStringRef(buffer));
  }

  ConfigureContext getContext(llvm::SMRange at) {
    BuildFileToken atToken{
        at.Start.getPointer(),
        unsigned(at.End.getPointer() - at.Start.getPointer())};
    return ConfigureContext{delegate, mainFilename, atToken};
  }

  ConfigureContext getContext() {
    return ConfigureContext{delegate, mainFilename, {}};
  }

  // FIXME: Factor out into a parser helper class.
  StringRef stringRefFromNode(flexbuffers::Reference ref) {
    auto string = ref.AsString();
    return {string.c_str(), string.size()};
  }
  StringRef stringRefFromNode(const flatbuffers::String& string) {
    return {string.c_str(), string.size()};
  }

  std::vector<std::pair<StringRef, StringRef>>
  parseAttributeMap(flexbuffers::Reference ref) {
    const flexbuffers::Map map = ref.AsMap();
    std::vector<std::pair<StringRef, StringRef>> attributes;
    attributes.reserve(map.size());
    for (size_t i = 0; i < attributes.size(); ++i) {
      attributes.emplace_back(stringRefFromNode(map.Keys()[i]),
                              stringRefFromNode(map.Values()[i]));
    }
    return attributes;
  }

  std::vector<StringRef> parseAttributes(flexbuffers::Reference ref) {
    const flexbuffers::Vector vector = ref.AsVector();
    std::vector<StringRef> attributes;
    attributes.reserve(vector.size());
    for (size_t i = 0; i < attributes.size(); ++i) {
      attributes.emplace_back(stringRefFromNode(vector[i]));
    }
    return attributes;
  }

  Tool* getOrCreateTool(StringRef name) {
    // First, check the map.
    auto it = tools.find(name);
    if (it != tools.end())
      return it->second.get();

    // Otherwise, ask the delegate to create the tool.
    auto tool = delegate.lookupTool(name);
    if (!tool) {
      error("invalid tool type in 'tools' map");
      return nullptr;
    }
    auto result = tool.get();
    tools[name] = std::move(tool);

    return result;
  }

  Node* getOrCreateNode(StringRef name, bool isImplicit) {
    // First, check the map.
    auto it = nodes.find(name);
    if (it != nodes.end())
      return it->second.get();

    // Otherwise, ask the delegate to create the node.
    auto node = delegate.lookupNode(name, isImplicit);
    assert(node);
    auto result = node.get();
    nodes[name] = std::move(node);

    return result;
  }

  template <typename T>
  bool configureAttributes(flexbuffers::Reference ref, T& entity) {
    const auto map = ref.AsMap();
    for (size_t i = 0; i < map.size(); ++i) {
      StringRef attribute = stringRefFromNode(map.Keys()[i]);
      const auto value = map.Values()[i];

      if (value.IsMap()) {
        auto values = parseAttributeMap(value);
        if (!entity.configureAttribute(getContext(), attribute, values)) {
          return false;
        }
      } else if (value.IsVector()) {
        auto values = parseAttributes(value);
        if (!entity.configureAttribute(getContext(), attribute, values)) {
          return false;
        }
      } else if (value.IsString()) {
        if (!entity.configureAttribute(getContext(), attribute,
                                       stringRefFromNode(value))) {
          return false;
        }
      } else {
        error("invalid value type: expected map, vector or string");
        return false;
      }
    }
    return true;
  }

  bool parseBuildFile(const format::BuildFile* file) {
    // Parse the client mapping.
    if (!parseClientMapping(*file->client()))
      return false;

    // Parse the tools mapping, if present.
    if (file->tools() && !parseToolsMapping(*file->tools()))
      return false;

    // Parse the targets mapping, if present.
    if (file->targets() && !parseTargetsMapping(*file->targets()))
      return false;

    // Parse the default target, if present.
    if (file->default_() && !parseDefaultTarget(*file->default_()))
      return false;

    // Parse the nodes mapping, if present.
    if (file->nodes() && !parseNodesMapping(*file->nodes()))
      return false;

    // Parse the commands mapping, if present.
    if (file->commands() && !parseCommandsMapping(*file->commands()))
      return false;

    return true;
  }

  bool parseClientMapping(const format::Client& map) {
    // Collect all of the keys.
    const StringRef name = stringRefFromNode(*map.name());
    const uint32_t version = map.version();
    property_list_type properties;
    if (map.properties()) {
      auto attributeMap = parseAttributeMap(map.properties_flexbuffer_root());
      properties.assign(attributeMap.begin(), attributeMap.end());
    }

    // Pass to the delegate.
    if (!delegate.configureClient(getContext(), name, version, properties)) {
      error("unable to configure client");
      return false;
    }

    return true;
  }

  bool parseToolsMapping(
      const flatbuffers::Vector<flatbuffers::Offset<format::Tool>>& map) {
    for (const format::Tool* entry : map) {
      StringRef name = stringRefFromNode(*entry->name());

      // Get the tool.
      auto tool = getOrCreateTool(name);
      if (!tool) {
        return false;
      }

      // Configure all of the tool attributes.
      if (entry->properties() &&
          !configureAttributes(entry->properties_flexbuffer_root(), *tool)) {
        return false;
      }
    }

    return true;
  }

  bool parseTargetsMapping(
      const flatbuffers::Vector<flatbuffers::Offset<format::Target>>& map) {
    for (const format::Target* entry : map) {
      StringRef name = stringRefFromNode(*entry->name());

      // Create the target.
      auto target = llvm::make_unique<Target>(name);

      // Add all of the nodes.
      for (const auto& node : *entry->commands()) {
        target->getNodes().push_back(getOrCreateNode(stringRefFromNode(*node),
                                                     /*isImplicit=*/true));
      }

      // Let the delegate know we loaded a target.
      delegate.loadedTarget(name, *target);

      // Add the target to the targets map.
      targets[name] = std::move(target);
    }

    return true;
  }

  bool parseDefaultTarget(const flatbuffers::String& entry) {
    StringRef target = stringRefFromNode(entry);

    if (targets.find(target) == targets.end()) {
      error("invalid default target, a default target should be in targets");
      return false;
    }

    defaultTarget = target;
    delegate.loadedDefaultTarget(defaultTarget);

    return true;
  }

  bool parseNodesMapping(
      const flatbuffers::Vector<flatbuffers::Offset<format::Node>>& map) {
    for (const format::Node* entry : map) {
      StringRef name = stringRefFromNode(*entry->name());

      // Get the node.
      //
      // FIXME: One downside of doing the lookup here is that the client cannot
      // ever make a context dependent node that can have configured properties.
      auto node = getOrCreateNode(name, /*isImplicit=*/false);

      // Configure all of the node attributes.
      if (entry->properties() &&
          !configureAttributes(entry->properties_flexbuffer_root(), *node)) {
        return false;
      }
    }

    return true;
  }

  bool parseCommandsMapping(
      const flatbuffers::Vector<flatbuffers::Offset<format::Command>>& map) {
    for (const format::Command* entry : map) {
      StringRef name = stringRefFromNode(*entry->name());

      // Check that the command is not a duplicate.
      if (commands.count(name) != 0) {
        error(Twine("duplicate command in 'commands' map: ") + name);
        continue;
      }

      // Lookup the tool for this command.
      auto tool = getOrCreateTool(stringRefFromNode(*entry->tool()));
      if (!tool) {
        return false;
      }

      // Create the command.
      auto command = tool->createCommand(name);
      assert(command && "tool failed to create a command");

      if (entry->inputs()) {
        std::vector<Node*> nodes;
        nodes.reserve(entry->inputs()->size());
        for (const auto& nodeName : *entry->inputs()) {
          nodes.push_back(getOrCreateNode(stringRefFromNode(*nodeName),
                                          /*isImplicit=*/true));
        }
        command->configureInputs(getContext(), nodes);
      }

      if (entry->outputs()) {
        std::vector<Node*> nodes;
        nodes.reserve(entry->outputs()->size());
        for (const auto& nodeName : *entry->outputs()) {
          auto* node = getOrCreateNode(stringRefFromNode(*nodeName),
                                       /*isImplicit=*/true);
          nodes.push_back(node);
          // Add this command to the node producer list.
          node->getProducers().push_back(command.get());
        }
        command->configureOutputs(getContext(), nodes);
      }

      if (entry->description()) {
        command->configureDescription(getContext(),
                                      stringRefFromNode(*entry->description()));
      }

      if (entry->properties() &&
          !configureAttributes(entry->properties_flexbuffer_root(), *command)) {
        return false;
      }

      // Let the delegate know we loaded a command.
      delegate.loadedCommand(name, *command);

      // Add the command to the commands map.
      commands[name] = std::move(command);
    }

    return true;
  }

public:
  BuildFileImpl(class BuildFile& buildFile, StringRef mainFilename,
                BuildFileDelegate& delegate)
      : mainFilename(mainFilename), delegate(delegate) {}

  BuildFileDelegate* getDelegate() { return &delegate; }

  /// @name Parse Actions
  /// @{

  std::unique_ptr<BuildDescription> load() {
    // Create a memory buffer for the input.
    //
    // FIXME: Lift the file access into the delegate, like we do for Ninja.
    llvm::SourceMgr sourceMgr;
    auto input = delegate.getFileSystem().getFileContents(mainFilename);
    if (!input) {
      error("unable to open '" + mainFilename + "'");
      return nullptr;
    }

    delegate.setFileContentsBeingParsed(input->getBuffer());

    const auto buffer =
        reinterpret_cast<const unsigned char*>(input->getBufferStart());
    const auto bufferSize = input->getBufferSize();

    if (!flatbuffers::Verifier{buffer, bufferSize}
             .VerifyBuffer<format::BuildFile>()) {
      error("invalid build file");
      return nullptr;
    }

    const format::BuildFile* file = format::GetBuildFile(buffer);
    if (!parseBuildFile(file)) {
      return nullptr;
    }

    // Create the actual description from our constructed elements.
    //
    // FIXME: This is historical, We should tidy up this class to reflect that
    // it is now just a builder.
    auto description = llvm::make_unique<BuildDescription>();
    std::swap(description->getNodes(), nodes);
    std::swap(description->getTargets(), targets);
    std::swap(description->getDefaultTarget(), defaultTarget);
    std::swap(description->getCommands(), commands);
    std::swap(description->getTools(), tools);
    return description;
  }
};

} // namespace

#pragma mark - BuildFile

BuildFile::BuildFile(StringRef mainFilename, BuildFileDelegate& delegate)
    : impl(new BuildFileImpl(*this, mainFilename, delegate)) {}

BuildFile::~BuildFile() { delete static_cast<BuildFileImpl*>(impl); }

std::unique_ptr<BuildDescription> BuildFile::load() {
  // Create the build description.
  return static_cast<BuildFileImpl*>(impl)->load();
}
