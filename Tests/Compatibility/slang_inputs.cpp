// Copyright (c) 2026 CodelineStudio
// SPDX-License-Identifier: MIT
#include <slang.h>
#include <slang-com-ptr.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {

using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

static std::string Read(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);

    if (!file)
        throw std::runtime_error("Cannot open input: " + path.string());

    std::string bytes(std::istreambuf_iterator<char>(file), {});

    if (file.bad())
        throw std::runtime_error("Cannot read input: " + path.string());

    return bytes;
}

static void Write(const fs::path& path, slang::IBlob* blob) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write((const char*)blob->getBufferPointer(), std::streamsize(blob->getBufferSize()));
    file.close();

    if (!file)
        throw std::runtime_error("Cannot write output: " + path.string());
}

static void Check(SlangResult result, Slang::ComPtr<slang::IBlob>& diagnostics, const char* operation) {
    bool emittedDiagnostics = diagnostics && diagnostics->getBufferSize();

    if (emittedDiagnostics)
        std::fwrite(diagnostics->getBufferPointer(), 1, diagnostics->getBufferSize(), stderr);

    diagnostics.setNull();

    // This qualification corpus is warning-clean. In particular, Slang may
    // return success after an optional optimizer failed to load: do not accept
    // a different compilation route as the pinned recipe's output.
    if (SLANG_FAILED(result) || emittedDiagnostics)
        throw std::runtime_error(operation);
}

// Only serialized artifacts in this deployed directory are visible to Slang.
// In particular, absolute original source paths stored in IR cannot be opened.
class ModuleFileSystem final : public ISlangFileSystem {
public:
    explicit ModuleFileSystem(fs::path root) : m_Root(fs::canonical(root)) {}

    SLANG_NO_THROW void* SLANG_MCALL castAs(const SlangUUID& id) override {
        const SlangUUID ids[] = {ISlangUnknown::getTypeGuid(), ISlangCastable::getTypeGuid(), ISlangFileSystem::getTypeGuid()};

        for (const auto& supported : ids) {
            if (std::memcmp(&id, &supported, sizeof(id)) == 0)
                return static_cast<ISlangFileSystem*>(this);
        }

        return nullptr;
    }

    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(const SlangUUID& id, void** output) override {
        *output = castAs(id);

        if (!*output)
            return SLANG_E_NO_INTERFACE;

        addRef();

        return SLANG_OK;
    }

    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override { return ++m_References; }

    SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
        uint32_t remaining = --m_References;

        if (!remaining)
            delete this;

        return remaining;
    }

    SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(const char* name, ISlangBlob** output) override {
        *output = nullptr;
        try {
            fs::path path = fs::absolute(name).lexically_normal();

            if (path.extension() != ".slang-module" || path.parent_path() != m_Root) {
                deniedReads++;

                return SLANG_E_NOT_FOUND;
            }

            std::string bytes = Read(path);
            *output = slang_createBlob(bytes.data(), bytes.size());
            moduleReads++;

            return *output ? SLANG_OK : SLANG_FAIL;
        } catch (...) {
            return SLANG_E_NOT_FOUND;
        }
    }

    uint32_t moduleReads = 0, deniedReads = 0;

private:
    fs::path m_Root;
    std::atomic<uint32_t> m_References{1};
};

static double Milliseconds(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - begin).count();
}

static int Compile(int argc, char** argv) {
    if (argc != 8)
        throw std::runtime_error("Usage: NRICompatibilitySlangInputs text|memory|module input entry compute|vertex|fragment output-prefix module-directory profile");

    std::string kind = argv[1];

    if (kind != "text" && kind != "memory" && kind != "module")
        throw std::runtime_error("Unknown input kind");

    SlangStage stage = std::strcmp(argv[4], "compute") == 0 ? SLANG_STAGE_COMPUTE : (std::strcmp(argv[4], "vertex") == 0 ? SLANG_STAGE_VERTEX : (std::strcmp(argv[4], "fragment") == 0 ? SLANG_STAGE_FRAGMENT : SLANG_STAGE_NONE));

    if (stage == SLANG_STAGE_NONE)
        throw std::runtime_error("Unknown shader stage");

    fs::path input = fs::absolute(argv[2]), prefix = fs::absolute(argv[5]), moduleDirectory = fs::absolute(argv[6]);
    fs::create_directories(prefix.parent_path());
    fs::create_directories(moduleDirectory);
    std::string moduleName = input.stem().string(), search = (kind == "module" ? moduleDirectory : input.parent_path()).string();
    const char* searchPaths[] = {search.c_str()};
    Slang::ComPtr<slang::IGlobalSession> global;
    Slang::ComPtr<slang::IBlob> diagnostics;
    auto start = Clock::now();
    Check(slang::createGlobalSession(global.writeRef()), diagnostics, "global session");

    if (std::strcmp(global->getBuildTagString(), "2026.21") != 0)
        throw std::runtime_error("Qualification requires Slang2026.21");

    slang::TargetDesc target = {};
    target.format = SLANG_SPIRV;
    target.profile = global->findProfile(argv[7]);

    if (!target.profile)
        throw std::runtime_error("Unknown target profile");

    slang::CompilerOptionEntry options[] = {
        {slang::CompilerOptionName::EmitSpirvDirectly, {slang::CompilerOptionValueKind::Int, 1}},
        {slang::CompilerOptionName::VulkanUseEntryPointName, {slang::CompilerOptionValueKind::Int, 1}},
        {slang::CompilerOptionName::Optimization, {slang::CompilerOptionValueKind::Int, SLANG_OPTIMIZATION_LEVEL_HIGH}},
        {slang::CompilerOptionName::DebugInformation, {slang::CompilerOptionValueKind::Int, SLANG_DEBUG_INFO_LEVEL_NONE}}
    };
    slang::SessionDesc desc = {};
    desc.targets = &target;
    desc.targetCount = 1;
    desc.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    desc.searchPaths = searchPaths;
    desc.searchPathCount = 1;
    desc.compilerOptionEntries = options;
    desc.compilerOptionEntryCount = sizeof(options) / sizeof(options[0]);
    Slang::ComPtr<ModuleFileSystem> moduleFiles;

    if (kind == "module") {
        moduleFiles.attach(new ModuleFileSystem(moduleDirectory));
        desc.fileSystem = moduleFiles;
    }

    Slang::ComPtr<slang::ISession> session;
    Check(global->createSession(desc, session.writeRef()), diagnostics, "session");
    Slang::ComPtr<slang::IModule> module;
    auto initialized = Clock::now();

    if (kind == "text")
        module = session->loadModule(moduleName.c_str(), diagnostics.writeRef());
    else {
        std::string bytes = Read(input);

        if (kind == "memory")
            module = session->loadModuleFromSourceString(moduleName.c_str(), input.string().c_str(), bytes.c_str(), diagnostics.writeRef());
        else {
            Slang::ComPtr<slang::IBlob> blob(Slang::INIT_ATTACH, slang_createBlob(bytes.data(), bytes.size()));
            module = session->loadModuleFromIRBlob(moduleName.c_str(), input.string().c_str(), blob, diagnostics.writeRef());
        }
    }

    Check(module ? SLANG_OK : SLANG_FAIL, diagnostics, "module load");
    Slang::ComPtr<slang::IEntryPoint> entry;
    Check(module->findAndCheckEntryPoint(argv[3], stage, entry.writeRef(), diagnostics.writeRef()), diagnostics, "entry point");
    auto loaded = Clock::now();

    if (kind != "module") {
        for (SlangInt i = 0; i < session->getLoadedModuleCount(); i++) {
            auto* loadedModule = session->getLoadedModule(i);
            Slang::ComPtr<slang::IBlob> serialized;
            Check(loadedModule->serialize(serialized.writeRef()), diagnostics, "module serialization");
            Write(moduleDirectory / (std::string(loadedModule->getName()) + ".slang-module"), serialized);
        }
    }

    auto serialized = Clock::now();
    slang::IComponentType* components[] = {module, entry};
    Slang::ComPtr<slang::IComponentType> composed, linked;
    Check(session->createCompositeComponentType(components, 2, composed.writeRef(), diagnostics.writeRef()), diagnostics, "composition");
    Check(composed->link(linked.writeRef(), diagnostics.writeRef()), diagnostics, "link");
    auto linkedTime = Clock::now();
    Slang::ComPtr<slang::IBlob> binary, reflection;
    Check(linked->getEntryPointCode(0, 0, binary.writeRef(), diagnostics.writeRef()), diagnostics, "code generation");
    auto* layout = linked->getLayout(0, diagnostics.writeRef());
    Check(layout ? SLANG_OK : SLANG_FAIL, diagnostics, "reflection layout");
    Check(layout->toJson(reflection.writeRef()), diagnostics, "reflection JSON");
    auto generated = Clock::now();
    Write(prefix.string() + ".spv", binary);
    Write(prefix.string() + ".reflection.json", reflection);
    std::printf("{\"kind\":\"%s\",\"compiler\":\"%s\",\"loaded_modules\":%lld,\"module_reads\":%u,\"denied_reads\":%u,\"initialize_ms\":%.3f,\"load_ms\":%.3f,\"serialize_ms\":%.3f,\"link_ms\":%.3f,\"codegen_ms\":%.3f}\n", kind.c_str(), global->getBuildTagString(), (long long)session->getLoadedModuleCount(), moduleFiles ? moduleFiles->moduleReads : 0, moduleFiles ? moduleFiles->deniedReads : 0, Milliseconds(start, initialized), Milliseconds(initialized, loaded), Milliseconds(loaded, serialized), Milliseconds(serialized, linkedTime), Milliseconds(linkedTime, generated));

    return std::fflush(stdout) == 0 ? 0 : 3;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return Compile(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Slang input qualification failed: %s\n", error.what());

        return 1;
    }
}
