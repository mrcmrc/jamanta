#ifdef USE_ORCJIT

namespace llvm {
namespace orc {
    /// @brief Simple compile functor: Takes a single IR module and returns an
    ///        ObjectFile.
    class PersistentSimpleCompiler {
        typedef object::OwningBinary<object::ObjectFile> OwningObj;
    public:
        /// @brief Construct a simple compile functor with the given target.
        PersistentSimpleCompiler(TargetMachine &TM) : TM(TM),
            ObjBufferSV(new SmallVector<char, 0>),
            ObjStream(new raw_svector_ostream(*ObjBufferSV)),
            PM(new legacy::PassManager) {
            if (TM.addPassesToEmitMC(*PM, Ctx, *ObjStream))
                llvm_unreachable("Target does not support MC emission.");
        }

        ~PersistentSimpleCompiler() {
            delete ObjBufferSV;
            delete ObjStream;
            delete PM;
        }

        /// @brief Compile a Module to an ObjectFile.
        object::OwningBinary<object::ObjectFile> operator()(Module &M) {
            PM->run(M);
            std::unique_ptr<MemoryBuffer> ObjBuffer(
                new ObjectMemoryBuffer(std::move(*ObjBufferSV)));
            ErrorOr<std::unique_ptr<object::ObjectFile>> Obj =
                object::ObjectFile::createObjectFile(ObjBuffer->getMemBufferRef());

            // TODO: Actually report errors helpfully.
            if (Obj)
                return OwningObj(std::move(*Obj), std::move(ObjBuffer));
            return OwningObj(nullptr, nullptr);
        }

    private:
        TargetMachine &TM;
        SmallVector<char, 0> *ObjBufferSV;
        raw_svector_ostream *ObjStream;
        legacy::PassManager *PM;
        MCContext *Ctx;
    };
}
}


template <typename T>
static std::vector<T> singletonSet(T t) {
  std::vector<T> Vec;
  Vec.push_back(std::move(t));
  return Vec;
}

class JuliaOJIT {
public:
    typedef orc::ObjectLinkingLayer<> ObjLayerT;
    typedef orc::IRCompileLayer<ObjLayerT> CompileLayerT;
    typedef CompileLayerT::ModuleSetHandleT ModuleHandleT;
    typedef StringMap<void*> GlobalSymbolTableT;

    JuliaOJIT(TargetMachine &TM)
      : TM(TM),
        DL(TM.createDataLayout()),
        CompileLayer(ObjectLayer, orc::PersistentSimpleCompiler(TM)) {
            // Make sure SectionMemoryManager::getSymbolAddressInProcess can resolve
            // symbols in the program as well. The nullptr argument to the function
            // tells DynamicLibrary to load the program, not a library.
            std::string *ErrorStr = nullptr;
            if (sys::DynamicLibrary::LoadLibraryPermanently(nullptr, ErrorStr))
                report_fatal_error("FATAL: unable to dlopen self\n" + *ErrorStr);
        }

    std::string mangle(const std::string &Name) {
        std::string MangledName;
        {
            raw_string_ostream MangledNameStream(MangledName);
            Mangler::getNameWithPrefix(MangledNameStream, Name, DL);
        }
        return MangledName;
    }

    void addGlobalMapping(StringRef Name, void *Addr) {
       GlobalSymbolTable[mangle(Name)] = Addr;
    }

    ModuleHandleT addModule(Module *M) {
        // We need a memory manager to allocate memory and resolve symbols for this
        // new module. Create one that resolves symbols by looking back into the
        // JIT.
        auto Resolver = orc::createLambdaResolver(
                          [&](const std::string &Name) {
                            // TODO: consider moving the FunctionMover resolver here
                            // Step 0: ObjectLinkingLayer has checked whether it is in the current module
                            // Step 1: Check against list of known external globals
                            GlobalSymbolTableT::const_iterator pos = GlobalSymbolTable.find(Name);
                            if (pos != GlobalSymbolTable.end())
                                return RuntimeDyld::SymbolInfo((intptr_t)pos->second, JITSymbolFlags::Exported);
                            // Step 2: Search all previously emitted symbols
                            if (auto Sym = findSymbol(Name))
                              return RuntimeDyld::SymbolInfo(Sym.getAddress(),
                                                             Sym.getFlags());
                            // Step 2: Search the program symbols
                            if (uint64_t addr = SectionMemoryManager::getSymbolAddressInProcess(Name))
                                return RuntimeDyld::SymbolInfo(addr, JITSymbolFlags::Exported);
                            // Return failure code
                            return RuntimeDyld::SymbolInfo(nullptr);
                          },
                          [](const std::string &S) { return nullptr; }
                        );
        return CompileLayer.addModuleSet(singletonSet(std::move(M)),
                                         &MemMgr,
                                         std::move(Resolver));
    }

    void removeModule(ModuleHandleT H) { CompileLayer.removeModuleSet(H); }

    orc::JITSymbol findSymbol(const std::string &Name) {
        return CompileLayer.findSymbol(Name, true);
    }

    orc::JITSymbol findUnmangledSymbol(const std::string Name) {
        return findSymbol(mangle(Name));
    }

    uint64_t getGlobalValueAddress(const std::string &Name) {
        return CompileLayer.findSymbol(mangle(Name), false).getAddress();
    }

    uint64_t getFunctionAddress(const std::string &Name) {
        return CompileLayer.findSymbol(mangle(Name), false).getAddress();
    }

    uint64_t FindFunctionNamed(const std::string &Name) {
        return 0; // Functions are not kept around
    }

    void RegisterJITEventListener(JITEventListener *L) {
        // TODO
    }

    const DataLayout& getDataLayout() const {
        return DL;
    }

    const Triple& getTargetTriple() const {
        return TM.getTargetTriple();
    }

private:
    TargetMachine &TM;
    const DataLayout DL;
    SectionMemoryManager MemMgr;
    ObjLayerT ObjectLayer;
    CompileLayerT CompileLayer;
    GlobalSymbolTableT GlobalSymbolTable;
};
#endif
