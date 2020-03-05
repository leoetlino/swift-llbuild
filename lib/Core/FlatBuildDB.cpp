#include "llbuild/Core/BuildDB.h"

#include "llbuild/Basic/PlatformUtility.h"
#include "llbuild/Core/BuildEngine.h"
#include "llbuild/Core/FlatBuildDBFormat.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <absl/container/flat_hash_map.h>

#include <cassert>
#include <fstream>
#include <mutex>
#include <variant>

namespace llbuild::core {

namespace detail {

class FlatBuildDB : public BuildDB {
  std::string path;
  uint32_t clientSchemaVersion;
  /// If this is `true`, the database will be re-created if the client/schema
  /// version mismatches. If `false`, it will not be re-created but returns an
  /// error instead.
  bool recreateOnUnmatchedVersion;

  std::mutex dbMutex;
  std::unique_ptr<llvm::MemoryBuffer> dbData;
  absl::flat_hash_map<std::string_view,
                      std::variant<const format::RuleResult*, Result>>
      dbResults;
  uint32_t dbVersion;
  uint64_t dbIteration;
  bool dbDirty = false;

  /// The delegate pointer
  BuildDBDelegate* delegate = nullptr;

  bool doLoad(const format::BuildDB* db, std::string* error_out) {
    dbVersion = db->info()->client_version();
    dbIteration = db->info()->iteration();

    dbResults.reserve(db->results()->size());
    for (const format::RuleResult* result : *db->results()) {
      dbResults.emplace(result->key()->string_view(), result);
    }

    return true;
  }

  bool doOpen(std::string* error_out) {
    {
      auto data = llvm::MemoryBuffer::getFile(path);
      if (!data) {
        return true;
      }
      dbData = std::move(data.get());
    }

    auto* dataPtr = reinterpret_cast<const uint8_t*>(dbData->getBufferStart());
#ifndef NDEBUG
    flatbuffers::Verifier verifier{dataPtr, data.get()->getBufferSize()};
    if (!format::VerifyBuildDBBuffer(verifier)) {
      *error_out = "database verification failed";
      return false;
    }
#endif
    return doLoad(format::GetBuildDB(dataPtr), error_out);
  }

  bool open(std::string* error_out) {
    if (dbData)
      return true;

    if (!doOpen(error_out))
      return false;

    if (dbVersion != clientSchemaVersion) {
      if (!recreateOnUnmatchedVersion) {
        // We don't re-create the database in this case and return an error
        *error_out = std::string("Version mismatch. (") +
                     std::string("database-client: ") +
                     std::to_string(dbVersion) +
                     std::string(" requested client: ") +
                     std::to_string(clientSchemaVersion) + std::string(")");
        return false;
      }

      // Always recreate the database from scratch when the schema changes.
      int result = basic::sys::unlink(path.c_str());
      if (result == -1) {
        if (errno != ENOENT) {
          *error_out = std::string("unable to unlink existing database: ") +
                       ::strerror(errno);
          return false;
        }
      } else {
        dbData = nullptr;
        if (!doOpen(error_out))
          return false;
      }
    }

    return true;
  }

  bool hasResultChanged(const Result& result,
                        const format::RuleResult* oldResult) const {
    const auto depsChanged = [&] {
      const size_t n = result.dependencies.size();
      const auto& oldKeys = *oldResult->dependencies()->keys();
      const auto& oldFlags = *oldResult->dependencies()->flags();

      if (n != oldFlags.size())
        return true;

      for (size_t i = 0; i < n; ++i) {
        if (result.dependencies[i].flag != oldFlags[i])
          return true;
        // getKeyForID is less expensive than the reverse operation, which
        // possibly requires inserting a new entry into a StringMap.
        if (delegate->getKeyForID(result.dependencies[i].keyID) !=
            oldKeys[i]->string_view()) {
          return true;
        }
      }

      return false;
    };

    return !std::equal(result.value.begin(), result.value.end(),
                       oldResult->value()->begin(),
                       oldResult->value()->end()) ||
           result.signature.value != oldResult->signature() || depsChanged();
  }

public:
  FlatBuildDB(StringRef path, uint32_t clientSchemaVersion,
              bool recreateOnUnmatchedVersion)
      : path(path), clientSchemaVersion(clientSchemaVersion),
        recreateOnUnmatchedVersion(recreateOnUnmatchedVersion) {}

  ~FlatBuildDB() override = default;

  /// @name BuildDB API
  /// @{

  void attachDelegate(BuildDBDelegate* delegate) override {
    this->delegate = delegate;
  }

  Epoch getCurrentEpoch(bool* success_out, std::string* error_out) override {
    std::lock_guard<std::mutex> guard(dbMutex);

    if (!open(error_out)) {
      *success_out = false;
      return 0;
    }

    *success_out = true;
    return dbIteration;
  }

  bool setCurrentIteration(uint64_t value, std::string* error_out) override {
    std::lock_guard<std::mutex> guard(dbMutex);

    if (!open(error_out)) {
      return false;
    }

    dbIteration = value;
    return true;
  }

  bool lookupRuleResult(KeyID keyID, const KeyType& key, Result* result_out,
                        std::string* error_out) override {
    std::lock_guard<std::mutex> guard(dbMutex);
    assert(result_out->builtAt == 0);

    if (!open(error_out)) {
      return false;
    }

    const auto it = dbResults.find(key.str());
    if (it == dbResults.end())
      return false;

    if (auto result = std::get_if<Result>(&it->second)) {
      *result_out = *result;
    } else if (auto ptr = std::get_if<const format::RuleResult*>(&it->second)) {
      const format::RuleResult* result = *ptr;

      result_out->value.reserve(result->value()->size());
      result_out->value.assign(result->value()->begin(),
                               result->value()->end());
      result_out->signature = basic::CommandSignature(result->signature());
      result_out->computedAt = result->computed_at();
      result_out->builtAt = result->built_at();
      result_out->start = result->start();
      result_out->end = result->end();

      const auto& depKeys = *result->dependencies()->keys();
      const auto& depFlags = *result->dependencies()->flags();
      if (depKeys.size() != depFlags.size()) {
        *error_out = "invalid dependency data";
        return false;
      }
      result_out->dependencies.resize(depKeys.size());
      for (size_t i = 0; i < depKeys.size(); ++i) {
        result_out->dependencies.set(
            i, delegate->getKeyID(depKeys[i]->string_view()), depFlags[i]);
      }
    }
    return true;
  }

  bool setRuleResult(KeyID keyID, const Rule& rule, const Result& ruleResult,
                     std::string* error_out) override {
    std::lock_guard<std::mutex> guard(dbMutex);

    if (!open(error_out)) {
      return false;
    }

    auto it = dbResults.find(rule.key.str());
    if (it == dbResults.end()) {
      dbDirty = true;
      dbResults.emplace(rule.key.str(), ruleResult);
    } else {
      // No need to update dbDirty if the variant holds a Result,
      // since that means a new result was added and dbDirty is already true.
      if (auto old = std::get_if<const format::RuleResult*>(&it->second)) {
        if (hasResultChanged(ruleResult, *old)) {
          dbDirty = true;
        }
      }
      dbResults.insert_or_assign(rule.key.str(), ruleResult);
    }

    return true;
  }

  bool buildStarted(std::string* error_out) override { return true; }

  void buildComplete() override {
    std::lock_guard<std::mutex> guard(dbMutex);

    if (!dbDirty) {
      return;
    }

    flatbuffers::FlatBufferBuilder fbb(100'000'000);

    std::vector<flatbuffers::Offset<format::RuleResult>> results;
    results.reserve(dbResults.size());
    for (const auto& entry : dbResults) {
      if (std::holds_alternative<Result>(entry.second)) {
        const auto& result = std::get<Result>(entry.second);

        std::vector<flatbuffers::Offset<flatbuffers::String>> depKeys;
        std::vector<uint8_t> depFlags;
        depKeys.reserve(result.dependencies.size());
        depFlags.reserve(result.dependencies.size());
        for (auto keyIDAndFlag : result.dependencies) {
          depKeys.emplace_back(fbb.CreateString(
              delegate->getKeyForID(keyIDAndFlag.keyID).strv()));
          depFlags.emplace_back(keyIDAndFlag.flag);
        }

        const auto dependencies =
            format::CreateDependenciesDirect(fbb, &depKeys, &depFlags);

        results.emplace_back(format::CreateRuleResult(
            fbb, fbb.CreateString(entry.first), fbb.CreateVector(result.value),
            result.signature.value, result.builtAt, result.computedAt,
            result.start, result.end, dependencies));
      } else {
        const auto* result = std::get<const format::RuleResult*>(entry.second);

        std::vector<flatbuffers::Offset<flatbuffers::String>> depKeys;
        depKeys.reserve(result->dependencies()->keys()->size());
        for (const auto& key : *result->dependencies()->keys())
          depKeys.emplace_back(fbb.CreateString(key));

        const auto dependencies = format::CreateDependencies(
            fbb, fbb.CreateVector(depKeys),
            fbb.CreateVector(result->dependencies()->flags()->data(),
                             result->dependencies()->flags()->size()));

        results.emplace_back(format::CreateRuleResult(
            fbb, fbb.CreateString(result->key()),
            fbb.CreateVector(result->value()->data(), result->value()->size()),
            result->signature(), result->built_at(), result->computed_at(),
            result->start(), result->end(), dependencies));
      }
    }

    const format::Info info{clientSchemaVersion, dbIteration};
    fbb.Finish(format::CreateBuildDBDirect(fbb, &info, &results));

    std::ofstream file(path, std::ios::binary);
    file.write((const char*)fbb.GetBufferPointer(), fbb.GetSize());
  }

  bool getKeys(std::vector<KeyType>&, std::string* error_out) override {
    // FIXME: unimplemented
    *error_out = "unimplemented";
    return false;
  }

  bool getKeysWithResult(std::vector<KeyType>&, std::vector<Result>&,
                         std::string* error_out) override {
    // FIXME: unimplemented
    *error_out = "unimplemented";
    return false;
  }

  void dump(raw_ostream& os) override {
    // FIXME: unimplemented
    os << "unimplemented";
  }

  /// @}
};

} // namespace detail

std::unique_ptr<BuildDB> createFlatBuildDB(StringRef path,
                                           uint32_t clientSchemaVersion,
                                           bool recreateUnmatchedVersion,
                                           std::string* error_out) {
  return llvm::make_unique<detail::FlatBuildDB>(path, clientSchemaVersion,
                                                recreateUnmatchedVersion);
}

} // namespace llbuild::core
