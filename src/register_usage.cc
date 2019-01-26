
#include <cstddef>
#include <fstream>
#include <string>

#include <errno.h>

#if defined(__GLIBCXX__) || defined(__GLIBCPP__)
#include <ext/stdio_filebuf.h>
#else
#error We require libstdc++ at the moment. Compile with GCC or specify\
 libstdc++ at compile time (e.g: -stdlib=libstdc++ in Clang).
#endif

#include <sys/file.h>

#include "BPatch.h"
#include "BPatch_function.h"
#include "BPatch_object.h"
#include "CodeObject.h"
#include "glog/logging.h"
#include "register_usage.h"
#include "utils.h"

std::string NormalizeRegisterName(std::string reg) {
  if (!reg.compare(0, 1, "E")) {
    return "R" + reg.substr(1);
  }

  if (!(reg.compare("AX") && reg.compare("AH") && reg.compare("AL"))) {
    return "RAX";
  } else if (!(reg.compare("BX") && reg.compare("BH") && reg.compare("BL"))) {
    return "RBX";
  } else if (!(reg.compare("CX") && reg.compare("CH") && reg.compare("CL"))) {
    return "RCX";
  } else if (!(reg.compare("DX") && reg.compare("DH") && reg.compare("DL"))) {
    return "RDX";
  } else if (!reg.compare("SI")) {
    return "RSI";
  } else if (!reg.compare("DI")) {
    return "RDI";
  } else if (!reg.compare("BP")) {
    return "RBP";
  } else if (!reg.compare("SP")) {
    return "RSP";
  } else if (!reg.compare(0, 1, "R") && isdigit(reg.substr(1, 1).at(0)) &&
             (std::isalpha(reg.substr(reg.size() - 1).at(0)))) {
    if (std::isdigit(reg.substr(2, 1).at(0))) {
      return "R" + reg.substr(1, 2);
    }

    return "R" + reg.substr(1, 1);
  }

  return reg;
}

int ExtractNumericPostFix(std::string reg) {
  std::string post_fix =
      reg.substr(reg.size() - 2);  // Numeric Postfix can be one or two digits

  if (std::isdigit(post_fix[0])) {
    return std::stoi(post_fix, nullptr);
  } else if (std::isdigit(post_fix[1])) {
    return std::stoi(post_fix.substr(1), nullptr);
  }

  return -1;
}

void PopulateUnusedAvx2Mask(const std::set<std::string>& used,
                            RegisterUsageInfo* const info) {
  // Used register mask. Only 16 registers.
  bool used_mask[16] = {false};
  for (const std::string& reg : used) {
    if (!reg.compare(0, 3, "YMM") || !reg.compare(0, 3, "XMM")) {
      // Extract integer post fix
      int register_index = ExtractNumericPostFix(reg);
      DCHECK(register_index >= 0 && register_index < 16);
      used_mask[register_index] = true;
    }
  }

  for (int i = 0; i < 16; i++) {
    info->unused_avx2_mask.push_back(!used_mask[i]);
  }
}

void PopulateUnusedAvx512Mask(const std::set<std::string>& used,
                              RegisterUsageInfo* const info) {
  // Used register mask. 32 AVX512 registers
  bool used_mask[32] = {false};
  for (const std::string& reg : used) {
    if (!reg.compare(0, 3, "ZMM") || !reg.compare(0, 3, "YMM") ||
        !reg.compare(0, 3, "XMM")) {
      // Extract integer post fix
      int register_index = ExtractNumericPostFix(reg);
      DCHECK(register_index >= 0 && register_index < 32);
      used_mask[register_index] = true;
    }
  }

  for (int i = 0; i < 32; i++) {
    info->unused_avx512_mask.push_back(!used_mask[i]);
  }
}

void PopulateUnusedMmxMask(const std::set<std::string>& used,
                           RegisterUsageInfo* const info) {
  // First check if FPU register stack is used anywhere. If it has been then we
  // cannot use MMX register mode since they overlap with the FPU stack
  // registers.
  for (const std::string& reg : used) {
    if (!reg.compare(0, 2, "FP") ||
        !reg.compare(0, 2, "ST")) {  // FPU register used

      for (int i = 0; i < 8; i++) {
        info->unused_mmx_mask.push_back(false);
      }
      return;
    }
  }

  bool used_mask[8] = {false};  // Used register mask
  for (const std::string& reg : used) {
    if (!reg.compare(0, 1, "M")) {  // Register is MMX
      int register_index = ExtractNumericPostFix(reg);
      DCHECK(register_index >= 0 && register_index < 16);
      used_mask[register_index] = true;
    }
  }

  for (int i = 0; i < 8; i++) {
    info->unused_mmx_mask.push_back(!used_mask[i]);
  }
}

void PopulateUnusedGprMask(const std::set<std::string>& used,
                           RegisterUsageInfo* const info) {
  // TODO(chamibuddhika) Complete this
}

void PopulateUsedRegisters(Dyninst::ParseAPI::CodeObject* code_object,
                           std::set<std::string>& used) {
  code_object->parse();

  auto fit = code_object->funcs().begin();
  for (; fit != code_object->funcs().end(); ++fit) {
    Dyninst::ParseAPI::Function* f = *fit;

    printf("Function : %s\n", f->name().c_str());

    std::map<Dyninst::Offset, Dyninst::InstructionAPI::Instruction> insns;

    std::set<std::string> regs;

    for (auto b : f->blocks()) {
      b->getInsns(insns);

      for (auto const& ins : insns) {
        std::set<Dyninst::InstructionAPI::RegisterAST::Ptr> regsRead;
        std::set<Dyninst::InstructionAPI::RegisterAST::Ptr> regsWritten;
        ins.second.getReadSet(regsRead);
        ins.second.getWriteSet(regsWritten);

        for (auto it = regsRead.begin(); it != regsRead.end(); it++) {
          std::string normalized_name = NormalizeRegisterName((*it)->format());
          regs.insert(normalized_name);
          used.insert(normalized_name);
        }

        for (auto it = regsWritten.begin(); it != regsWritten.end(); it++) {
          std::string normalized_name = NormalizeRegisterName((*it)->format());
          regs.insert(normalized_name);
          used.insert(normalized_name);
        }
      }
    }

    PrintSequence<std::set<std::string>, std::string>(regs, ", ");
    printf("\n");
  }
}

const std::string kAuditCacheFile = ".audit_cache";

bool PopulateCacheFromDisk(
    std::map<std::string, std::set<std::string>>& cache) {
  std::ifstream cache_file(kAuditCacheFile);

  if (cache_file.fail()) {
    return false;
  }

  std::string line;
  while (std::getline(cache_file, line)) {
    std::vector<std::string> tokens = Split(line, ',');
    DCHECK(tokens.size() == 2);

    std::string library = tokens[0];
    printf("Loading library %s\n", library.c_str());
    printf(" %s\n\n", tokens[1].c_str());

    std::vector<std::string> registers = Split(tokens[1], ':');
    DCHECK(registers.size() > 0);

    std::set<std::string> register_set(registers.begin(), registers.end());
    cache[library] = register_set;
  }

  return true;
}

bool IsSharedLibrary(BPatch_object* object) {
  // TODO(chamibudhika) isSharedLib() should return false for program text
  // code object modules IMO. Check with Dyninst team about this.
  //
  //  std::vector<BPatch_module*> modules;
  //  object->modules(modules);
  //  DCHECK(modules.size() > 0);
  //
  //  return modules[0]->isSharedLib();

  // For now check if .so extension occurs somewhere in the object path
  std::string name = std::string(object->pathName());
  if (name.find(".so") != std::string::npos) {
    return true;
  }
  return false;
}

void FlushCacheToDisk(
    const std::map<std::string, std::set<std::string>>& cache) {
  std::ofstream cache_file(kAuditCacheFile);

  if (!cache_file.is_open()) {
    char error[2048];
    fprintf(stderr,
            "Failed to create/ open the register audit cache file with : %s\n",
            strerror_r(errno, error, 2048));
    return;
  }

  // Exclusively lock the file for writing
  int fd =
      static_cast<__gnu_cxx::stdio_filebuf<char>* const>(cache_file.rdbuf())
          ->fd();

  if (flock(fd, LOCK_EX)) {
    char error[2048];
    fprintf(stderr, "Failed to lock the register audit cache file with : %s\n",
            strerror_r(errno, error, 2048));
    return;
  }

  for (auto const& it : cache) {
    std::string registers_concat = "";
    std::set<std::string> registers = it.second;

    for (auto const& reg : registers) {
      registers_concat += (reg + ":");
    }

    // Remove the trailing ':'
    registers_concat.pop_back();
    cache_file << it.first << "," << registers_concat << std::endl;
  }

  if (flock(fd, LOCK_UN)) {
    char error[2048];
    fprintf(stderr,
            "Failed to unlock the register audit cache file with : %s\n",
            strerror_r(errno, error, 2048));
  }

  cache_file.close();
  return;
}

RegisterUsageInfo GetUnusedRegisterInfo(std::string binary) {
  BPatch* bpatch = new BPatch;
  // Open binary and its linked shared libraries for parsing
  BPatch_addressSpace* app = bpatch->openBinary(binary.c_str(), true);
  BPatch_image* image = app->getImage();

  std::vector<BPatch_object*> objects;
  image->getObjects(objects);

  // Used registers in the application and its linked shared libraries
  std::set<std::string> used;
  // Register audit cache deserialized from the disk
  std::map<std::string, std::set<std::string>> cache;
  bool is_cache_present = PopulateCacheFromDisk(cache);

  for (auto it = objects.begin(); it != objects.end(); it++) {
    BPatch_object* object = *it;

    printf("Parsing object : %s\n", object->pathName().c_str());

    std::set<std::string> registers;
    if (is_cache_present && IsSharedLibrary(object)) {
      auto it = cache.find(object->pathName());
      if (it != cache.end()) {
        registers = it->second;
      }
    }

    if (!registers.size()) {
      // Couldn't find the library info in the cache or this object is the
      // program text. Parse the object and get register usage information.
      PopulateUsedRegisters(Dyninst::ParseAPI::convert(object), registers);

      // Update the cache if this is a shared library
      if (IsSharedLibrary(object)) {
        cache[object->pathName()] = registers;
      }
    }

    for (auto const& reg : registers) {
      used.insert(reg);
    }

    printf("\n\n");
  }

  FlushCacheToDisk(cache);

  PrintSequence<std::set<std::string>, std::string>(used, ", ");

  RegisterUsageInfo info;
  PopulateUnusedAvx2Mask(used, &info);
  PopulateUnusedAvx512Mask(used, &info);
  PopulateUnusedMmxMask(used, &info);

  return info;
}
