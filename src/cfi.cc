
#include <string.h>

#include "gflags/gflags.h"
#include "glog/logging.h"
#include "instrument.h"
#include "parse.h"
#include "register_usage.h"
#include "utils.h"

DEFINE_bool(vv, false, "Log verbose output.");

DEFINE_string(
    shadow_stack, "avx2",
    "\n Shadow stack implementation mechanism for backward-edge protection.\n"
    "\n Valid values are\n"
    "   * avx2 : Uses avx2 register file as backing store\n"
    "   * avx512 : Uses avx512 register file as backing store\n"
    "   * mem : Uses a memory region as backing store\n"
    "   * xor : Uses a xor check based technique to validate the return chain."
    " Less context sensitive and precise than other techniques.\n");

DEFINE_string(shadow_stack_protection, "sfi",
              "\n Applicable only when `shadow-stack` is set to mem."
              " Specifies protection mechanism for the memory region used as"
              " the backing store for the shadow stack.\n"
              "\n Valid values are\n"
              "   * sfi: Use Software Fault Isolation by sanitizing every"
              " memory write of the application\n"
              "   * mpx : Use mpx bound checking\n"
              "   * none : Use no protection\n");

DEFINE_string(cache, "./libs/",
              "\n Path to the cache of hardened shared libraries."
              " Once a shared library dependency is encountered the tool will"
              " check in the cache and reuse that if available.\n");

DEFINE_string(
    install, "./bin/",
    "\n Installation path of the hardened binary and its dependencies.");

static bool ValidateShadowStackFlag(const char* flagname,
                                    const std::string& value) {
  if (value == "avx2" || value == "avx512" || value == "mem") {
    return true;
  }
  return false;
}

static bool ValidateShadowStackProtectionFlag(const char* flagname,
                                              const std::string& value) {
  if (value == "sfi" || value == "mpx" || value == "none") {
    return true;
  }
  return false;
}

DEFINE_validator(shadow_stack, &ValidateShadowStackFlag);

DEFINE_validator(shadow_stack_protection, &ValidateShadowStackProtectionFlag);

void PrintVector(const std::vector<bool>& vec) {
  for (int i = 0; i < vec.size(); i++) {
    printf("%d", vec[i]);
  }
  printf("\n");
}

int main(int argc, char* argv[]) {
  google::InitGoogleLogging(argv[0]);

  std::string usage("Usage : ./cfi <flags> binary");
  gflags::SetUsageMessage(usage);

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  std::string binary(argv[1]);

  Parser parser = InitParser(binary);

  RegisterUsageInfo info = GetUnusedRegisterInfo(binary, parser);

  Instrument(binary, info, parser);

  return 0;
}
