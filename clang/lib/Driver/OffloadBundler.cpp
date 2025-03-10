//===- OffloadBundler.cpp - File Bundling and Unbundling ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements an offload bundling API that bundles different files
/// that relate with the same source code but different targets into a single
/// one. Also the implements the opposite functionality, i.e. unbundle files
/// previous created by this API.
///
//===----------------------------------------------------------------------===//

#include "clang/Driver/OffloadBundler.h"
#include "clang/Basic/Cuda.h"
#include "clang/Basic/TargetID.h"
#include "clang/Basic/Version.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/EndianStream.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <forward_list>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>

using namespace llvm;
using namespace llvm::object;
using namespace clang;

/// Magic string that marks the existence of offloading data.
#define OFFLOAD_BUNDLER_MAGIC_STR "__CLANG_OFFLOAD_BUNDLE__"

OffloadTargetInfo::OffloadTargetInfo(const StringRef Target,
                                     const OffloadBundlerConfig &BC)
    : BundlerConfig(BC) {

  // TODO: Add error checking from ClangOffloadBundler.cpp
  auto TargetFeatures = Target.split(':');
  auto TripleOrGPU = TargetFeatures.first.rsplit('-');

  if (clang::StringToCudaArch(TripleOrGPU.second) != clang::CudaArch::UNKNOWN) {
    auto KindTriple = TripleOrGPU.first.split('-');
    this->OffloadKind = KindTriple.first;

    // Enforce optional env field to standardize bundles
    llvm::Triple t = llvm::Triple(KindTriple.second);
    this->Triple = llvm::Triple(t.getArchName(), t.getVendorName(),
                                t.getOSName(), t.getEnvironmentName());

    this->TargetID = Target.substr(Target.find(TripleOrGPU.second));
  } else {
    auto KindTriple = TargetFeatures.first.split('-');
    this->OffloadKind = KindTriple.first;

    // Enforce optional env field to standardize bundles
    llvm::Triple t = llvm::Triple(KindTriple.second);
    this->Triple = llvm::Triple(t.getArchName(), t.getVendorName(),
                                t.getOSName(), t.getEnvironmentName());

    this->TargetID = "";
  }
}

bool OffloadTargetInfo::hasHostKind() const {
  return this->OffloadKind == "host";
}

bool OffloadTargetInfo::isOffloadKindValid() const {
  return OffloadKind == "host" || OffloadKind == "openmp" ||
         OffloadKind == "hip" || OffloadKind == "hipv4";
}

bool OffloadTargetInfo::isOffloadKindCompatible(
    const StringRef TargetOffloadKind) const {
  if (OffloadKind == TargetOffloadKind)
    return true;
  if (BundlerConfig.HipOpenmpCompatible) {
    bool HIPCompatibleWithOpenMP = OffloadKind.starts_with_insensitive("hip") &&
                                   TargetOffloadKind == "openmp";
    bool OpenMPCompatibleWithHIP =
        OffloadKind == "openmp" &&
        TargetOffloadKind.starts_with_insensitive("hip");
    return HIPCompatibleWithOpenMP || OpenMPCompatibleWithHIP;
  }
  return false;
}

bool OffloadTargetInfo::isTripleValid() const {
  return !Triple.str().empty() && Triple.getArch() != Triple::UnknownArch;
}

bool OffloadTargetInfo::operator==(const OffloadTargetInfo &Target) const {
  return OffloadKind == Target.OffloadKind &&
         Triple.isCompatibleWith(Target.Triple) && TargetID == Target.TargetID;
}

std::string OffloadTargetInfo::str() const {
  return Twine(OffloadKind + "-" + Triple.str() + "-" + TargetID).str();
}

static StringRef getDeviceFileExtension(StringRef Device,
                                        StringRef BundleFileName) {
  if (Device.contains("gfx"))
    return ".bc";
  if (Device.contains("sm_"))
    return ".cubin";
  return sys::path::extension(BundleFileName);
}

static std::string getDeviceLibraryFileName(StringRef BundleFileName,
                                            StringRef Device) {
  StringRef LibName = sys::path::stem(BundleFileName);
  StringRef Extension = getDeviceFileExtension(Device, BundleFileName);

  std::string Result;
  Result += LibName;
  Result += Extension;
  return Result;
}

/// @brief Checks if a code object \p CodeObjectInfo is compatible with a given
/// target \p TargetInfo.
/// @link https://clang.llvm.org/docs/ClangOffloadBundler.html#bundle-entry-id
bool isCodeObjectCompatible(const OffloadTargetInfo &CodeObjectInfo,
                            const OffloadTargetInfo &TargetInfo) {

  // Compatible in case of exact match.
  if (CodeObjectInfo == TargetInfo) {
    DEBUG_WITH_TYPE("CodeObjectCompatibility",
                    dbgs() << "Compatible: Exact match: \t[CodeObject: "
                           << CodeObjectInfo.str()
                           << "]\t:\t[Target: " << TargetInfo.str() << "]\n");
    return true;
  }

  // Incompatible if Kinds or Triples mismatch.
  if (!CodeObjectInfo.isOffloadKindCompatible(TargetInfo.OffloadKind) ||
      !CodeObjectInfo.Triple.isCompatibleWith(TargetInfo.Triple)) {
    DEBUG_WITH_TYPE(
        "CodeObjectCompatibility",
        dbgs() << "Incompatible: Kind/Triple mismatch \t[CodeObject: "
               << CodeObjectInfo.str() << "]\t:\t[Target: " << TargetInfo.str()
               << "]\n");
    return false;
  }

  // Incompatible if target IDs are incompatible.
  if (!clang::isCompatibleTargetID(CodeObjectInfo.TargetID,
                                   TargetInfo.TargetID)) {
    DEBUG_WITH_TYPE(
        "CodeObjectCompatibility",
        dbgs() << "Incompatible: target IDs are incompatible \t[CodeObject: "
               << CodeObjectInfo.str() << "]\t:\t[Target: " << TargetInfo.str()
               << "]\n");
    return false;
  }

  DEBUG_WITH_TYPE(
      "CodeObjectCompatibility",
      dbgs() << "Compatible: Code Objects are compatible \t[CodeObject: "
             << CodeObjectInfo.str() << "]\t:\t[Target: " << TargetInfo.str()
             << "]\n");
  return true;
}

namespace {
/// Generic file handler interface.
class FileHandler {
public:
  struct BundleInfo {
    StringRef BundleID;
  };

  FileHandler() {}

  virtual ~FileHandler() {}

  /// Update the file handler with information from the header of the bundled
  /// file.
  virtual Error ReadHeader(MemoryBuffer &Input) = 0;

  /// Read the marker of the next bundled to be read in the file. The bundle
  /// name is returned if there is one in the file, or `std::nullopt` if there
  /// are no more bundles to be read.
  virtual Expected<std::optional<StringRef>>
  ReadBundleStart(MemoryBuffer &Input) = 0;

  /// Read the marker that closes the current bundle.
  virtual Error ReadBundleEnd(MemoryBuffer &Input) = 0;

  /// Read the current bundle and write the result into the stream \a OS.
  virtual Error ReadBundle(raw_ostream &OS, MemoryBuffer &Input) = 0;

  /// Write the header of the bundled file to \a OS based on the information
  /// gathered from \a Inputs.
  virtual Error WriteHeader(raw_fd_ostream &OS,
                            ArrayRef<std::unique_ptr<MemoryBuffer>> Inputs) = 0;

  /// Write the marker that initiates a bundle for the triple \a TargetTriple to
  /// \a OS.
  virtual Error WriteBundleStart(raw_fd_ostream &OS,
                                 StringRef TargetTriple) = 0;

  /// Write the marker that closes a bundle for the triple \a TargetTriple to \a
  /// OS.
  virtual Error WriteBundleEnd(raw_fd_ostream &OS, StringRef TargetTriple) = 0;

  /// Write the bundle from \a Input into \a OS.
  virtual Error WriteBundle(raw_fd_ostream &OS, MemoryBuffer &Input) = 0;

  /// List bundle IDs in \a Input.
  virtual Error listBundleIDs(MemoryBuffer &Input) {
    if (Error Err = ReadHeader(Input))
      return Err;
    return forEachBundle(Input, [&](const BundleInfo &Info) -> Error {
      llvm::outs() << Info.BundleID << '\n';
      Error Err = listBundleIDsCallback(Input, Info);
      if (Err)
        return Err;
      return Error::success();
    });
  }

  /// For each bundle in \a Input, do \a Func.
  Error forEachBundle(MemoryBuffer &Input,
                      std::function<Error(const BundleInfo &)> Func) {
    while (true) {
      Expected<std::optional<StringRef>> CurTripleOrErr =
          ReadBundleStart(Input);
      if (!CurTripleOrErr)
        return CurTripleOrErr.takeError();

      // No more bundles.
      if (!*CurTripleOrErr)
        break;

      StringRef CurTriple = **CurTripleOrErr;
      assert(!CurTriple.empty());

      BundleInfo Info{CurTriple};
      if (Error Err = Func(Info))
        return Err;
    }
    return Error::success();
  }

protected:
  virtual Error listBundleIDsCallback(MemoryBuffer &Input,
                                      const BundleInfo &Info) {
    return Error::success();
  }
};

/// Handler for binary files. The bundled file will have the following format
/// (all integers are stored in little-endian format):
///
/// "OFFLOAD_BUNDLER_MAGIC_STR" (ASCII encoding of the string)
///
/// NumberOfOffloadBundles (8-byte integer)
///
/// OffsetOfBundle1 (8-byte integer)
/// SizeOfBundle1 (8-byte integer)
/// NumberOfBytesInTripleOfBundle1 (8-byte integer)
/// TripleOfBundle1 (byte length defined before)
///
/// ...
///
/// OffsetOfBundleN (8-byte integer)
/// SizeOfBundleN (8-byte integer)
/// NumberOfBytesInTripleOfBundleN (8-byte integer)
/// TripleOfBundleN (byte length defined before)
///
/// Bundle1
/// ...
/// BundleN

/// Read 8-byte integers from a buffer in little-endian format.
static uint64_t Read8byteIntegerFromBuffer(StringRef Buffer, size_t pos) {
  return llvm::support::endian::read64le(Buffer.data() + pos);
}

/// Write 8-byte integers to a buffer in little-endian format.
static void Write8byteIntegerToBuffer(raw_fd_ostream &OS, uint64_t Val) {
  llvm::support::endian::write(OS, Val, llvm::support::little);
}

class BinaryFileHandler final : public FileHandler {
  /// Information about the bundles extracted from the header.
  struct BinaryBundleInfo final : public BundleInfo {
    /// Size of the bundle.
    uint64_t Size = 0u;
    /// Offset at which the bundle starts in the bundled file.
    uint64_t Offset = 0u;

    BinaryBundleInfo() {}
    BinaryBundleInfo(uint64_t Size, uint64_t Offset)
        : Size(Size), Offset(Offset) {}
  };

  /// Map between a triple and the corresponding bundle information.
  StringMap<BinaryBundleInfo> BundlesInfo;

  /// Iterator for the bundle information that is being read.
  StringMap<BinaryBundleInfo>::iterator CurBundleInfo;
  StringMap<BinaryBundleInfo>::iterator NextBundleInfo;

  /// Current bundle target to be written.
  std::string CurWriteBundleTarget;

  /// Configuration options and arrays for this bundler job
  const OffloadBundlerConfig &BundlerConfig;

public:
  // TODO: Add error checking from ClangOffloadBundler.cpp
  BinaryFileHandler(const OffloadBundlerConfig &BC) : BundlerConfig(BC) {}

  ~BinaryFileHandler() final {}

  Error ReadHeader(MemoryBuffer &Input) final {
    StringRef FC = Input.getBuffer();

    // Initialize the current bundle with the end of the container.
    CurBundleInfo = BundlesInfo.end();

    // Check if buffer is smaller than magic string.
    size_t ReadChars = sizeof(OFFLOAD_BUNDLER_MAGIC_STR) - 1;
    if (ReadChars > FC.size())
      return Error::success();

    // Check if no magic was found.
    StringRef Magic(FC.data(), sizeof(OFFLOAD_BUNDLER_MAGIC_STR) - 1);
    if (!Magic.equals(OFFLOAD_BUNDLER_MAGIC_STR))
      return Error::success();

    // Read number of bundles.
    if (ReadChars + 8 > FC.size())
      return Error::success();

    uint64_t NumberOfBundles = Read8byteIntegerFromBuffer(FC, ReadChars);
    ReadChars += 8;

    // Read bundle offsets, sizes and triples.
    for (uint64_t i = 0; i < NumberOfBundles; ++i) {

      // Read offset.
      if (ReadChars + 8 > FC.size())
        return Error::success();

      uint64_t Offset = Read8byteIntegerFromBuffer(FC, ReadChars);
      ReadChars += 8;

      // Read size.
      if (ReadChars + 8 > FC.size())
        return Error::success();

      uint64_t Size = Read8byteIntegerFromBuffer(FC, ReadChars);
      ReadChars += 8;

      // Read triple size.
      if (ReadChars + 8 > FC.size())
        return Error::success();

      uint64_t TripleSize = Read8byteIntegerFromBuffer(FC, ReadChars);
      ReadChars += 8;

      // Read triple.
      if (ReadChars + TripleSize > FC.size())
        return Error::success();

      StringRef Triple(&FC.data()[ReadChars], TripleSize);
      ReadChars += TripleSize;

      // Check if the offset and size make sense.
      if (!Offset || Offset + Size > FC.size())
        return Error::success();

      assert(!BundlesInfo.contains(Triple) && "Triple is duplicated??");
      BundlesInfo[Triple] = BinaryBundleInfo(Size, Offset);
    }
    // Set the iterator to where we will start to read.
    CurBundleInfo = BundlesInfo.end();
    NextBundleInfo = BundlesInfo.begin();
    return Error::success();
  }

  Expected<std::optional<StringRef>>
  ReadBundleStart(MemoryBuffer &Input) final {
    if (NextBundleInfo == BundlesInfo.end())
      return std::nullopt;
    CurBundleInfo = NextBundleInfo++;
    return CurBundleInfo->first();
  }

  Error ReadBundleEnd(MemoryBuffer &Input) final {
    assert(CurBundleInfo != BundlesInfo.end() && "Invalid reader info!");
    return Error::success();
  }

  Error ReadBundle(raw_ostream &OS, MemoryBuffer &Input) final {
    assert(CurBundleInfo != BundlesInfo.end() && "Invalid reader info!");
    StringRef FC = Input.getBuffer();
    OS.write(FC.data() + CurBundleInfo->second.Offset,
             CurBundleInfo->second.Size);
    return Error::success();
  }

  Error WriteHeader(raw_fd_ostream &OS,
                    ArrayRef<std::unique_ptr<MemoryBuffer>> Inputs) final {

    // Compute size of the header.
    uint64_t HeaderSize = 0;

    HeaderSize += sizeof(OFFLOAD_BUNDLER_MAGIC_STR) - 1;
    HeaderSize += 8; // Number of Bundles

    for (auto &T : BundlerConfig.TargetNames) {
      HeaderSize += 3 * 8; // Bundle offset, Size of bundle and size of triple.
      HeaderSize += T.size(); // The triple.
    }

    // Write to the buffer the header.
    OS << OFFLOAD_BUNDLER_MAGIC_STR;

    Write8byteIntegerToBuffer(OS, BundlerConfig.TargetNames.size());

    unsigned Idx = 0;
    for (auto &T : BundlerConfig.TargetNames) {
      MemoryBuffer &MB = *Inputs[Idx++];
      HeaderSize = alignTo(HeaderSize, BundlerConfig.BundleAlignment);
      // Bundle offset.
      Write8byteIntegerToBuffer(OS, HeaderSize);
      // Size of the bundle (adds to the next bundle's offset)
      Write8byteIntegerToBuffer(OS, MB.getBufferSize());
      BundlesInfo[T] = BinaryBundleInfo(MB.getBufferSize(), HeaderSize);
      HeaderSize += MB.getBufferSize();
      // Size of the triple
      Write8byteIntegerToBuffer(OS, T.size());
      // Triple
      OS << T;
    }
    return Error::success();
  }

  Error WriteBundleStart(raw_fd_ostream &OS, StringRef TargetTriple) final {
    CurWriteBundleTarget = TargetTriple.str();
    return Error::success();
  }

  Error WriteBundleEnd(raw_fd_ostream &OS, StringRef TargetTriple) final {
    return Error::success();
  }

  Error WriteBundle(raw_fd_ostream &OS, MemoryBuffer &Input) final {
    auto BI = BundlesInfo[CurWriteBundleTarget];
    OS.seek(BI.Offset);
    OS.write(Input.getBufferStart(), Input.getBufferSize());
    return Error::success();
  }
};

// This class implements a list of temporary files that are removed upon
// object destruction.
class TempFileHandlerRAII {
public:
  ~TempFileHandlerRAII() {
    for (const auto &File : Files)
      sys::fs::remove(File);
  }

  // Creates temporary file with given contents.
  Expected<StringRef> Create(std::optional<ArrayRef<char>> Contents) {
    SmallString<128u> File;
    if (std::error_code EC =
            sys::fs::createTemporaryFile("clang-offload-bundler", "tmp", File))
      return createFileError(File, EC);
    Files.push_front(File);

    if (Contents) {
      std::error_code EC;
      raw_fd_ostream OS(File, EC);
      if (EC)
        return createFileError(File, EC);
      OS.write(Contents->data(), Contents->size());
    }
    return Files.front().str();
  }

private:
  std::forward_list<SmallString<128u>> Files;
};

/// Handler for object files. The bundles are organized by sections with a
/// designated name.
///
/// To unbundle, we just copy the contents of the designated section.
class ObjectFileHandler final : public FileHandler {

  /// The object file we are currently dealing with.
  std::unique_ptr<ObjectFile> Obj;

  /// Return the input file contents.
  StringRef getInputFileContents() const { return Obj->getData(); }

  /// Return bundle name (<kind>-<triple>) if the provided section is an offload
  /// section.
  static Expected<std::optional<StringRef>>
  IsOffloadSection(SectionRef CurSection) {
    Expected<StringRef> NameOrErr = CurSection.getName();
    if (!NameOrErr)
      return NameOrErr.takeError();

    // If it does not start with the reserved suffix, just skip this section.
    if (!NameOrErr->startswith(OFFLOAD_BUNDLER_MAGIC_STR))
      return std::nullopt;

    // Return the triple that is right after the reserved prefix.
    return NameOrErr->substr(sizeof(OFFLOAD_BUNDLER_MAGIC_STR) - 1);
  }

  /// Total number of inputs.
  unsigned NumberOfInputs = 0;

  /// Total number of processed inputs, i.e, inputs that were already
  /// read from the buffers.
  unsigned NumberOfProcessedInputs = 0;

  /// Iterator of the current and next section.
  section_iterator CurrentSection;
  section_iterator NextSection;

  /// Configuration options and arrays for this bundler job
  const OffloadBundlerConfig &BundlerConfig;

public:
  // TODO: Add error checking from ClangOffloadBundler.cpp
  ObjectFileHandler(std::unique_ptr<ObjectFile> ObjIn,
                    const OffloadBundlerConfig &BC)
      : Obj(std::move(ObjIn)), CurrentSection(Obj->section_begin()),
        NextSection(Obj->section_begin()), BundlerConfig(BC) {}

  ~ObjectFileHandler() final {}

  Error ReadHeader(MemoryBuffer &Input) final { return Error::success(); }

  Expected<std::optional<StringRef>>
  ReadBundleStart(MemoryBuffer &Input) final {
    while (NextSection != Obj->section_end()) {
      CurrentSection = NextSection;
      ++NextSection;

      // Check if the current section name starts with the reserved prefix. If
      // so, return the triple.
      Expected<std::optional<StringRef>> TripleOrErr =
          IsOffloadSection(*CurrentSection);
      if (!TripleOrErr)
        return TripleOrErr.takeError();
      if (*TripleOrErr)
        return **TripleOrErr;
    }
    return std::nullopt;
  }

  Error ReadBundleEnd(MemoryBuffer &Input) final { return Error::success(); }

  Error ReadBundle(raw_ostream &OS, MemoryBuffer &Input) final {
    Expected<StringRef> ContentOrErr = CurrentSection->getContents();
    if (!ContentOrErr)
      return ContentOrErr.takeError();
    StringRef Content = *ContentOrErr;

    // Copy fat object contents to the output when extracting host bundle.
    if (Content.size() == 1u && Content.front() == 0)
      Content = StringRef(Input.getBufferStart(), Input.getBufferSize());

    OS.write(Content.data(), Content.size());
    return Error::success();
  }

  Error WriteHeader(raw_fd_ostream &OS,
                    ArrayRef<std::unique_ptr<MemoryBuffer>> Inputs) final {
    assert(BundlerConfig.HostInputIndex != ~0u &&
           "Host input index not defined.");

    // Record number of inputs.
    NumberOfInputs = Inputs.size();
    return Error::success();
  }

  Error WriteBundleStart(raw_fd_ostream &OS, StringRef TargetTriple) final {
    ++NumberOfProcessedInputs;
    return Error::success();
  }

  Error WriteBundleEnd(raw_fd_ostream &OS, StringRef TargetTriple) final {
    assert(NumberOfProcessedInputs <= NumberOfInputs &&
           "Processing more inputs that actually exist!");
    assert(BundlerConfig.HostInputIndex != ~0u &&
           "Host input index not defined.");

    // If this is not the last output, we don't have to do anything.
    if (NumberOfProcessedInputs != NumberOfInputs)
      return Error::success();

    // We will use llvm-objcopy to add target objects sections to the output
    // fat object. These sections should have 'exclude' flag set which tells
    // link editor to remove them from linker inputs when linking executable or
    // shared library.

    assert(BundlerConfig.ObjcopyPath != "" &&
           "llvm-objcopy path not specified");

    // We write to the output file directly. So, we close it and use the name
    // to pass down to llvm-objcopy.
    OS.close();

    // Temporary files that need to be removed.
    TempFileHandlerRAII TempFiles;

    // Compose llvm-objcopy command line for add target objects' sections with
    // appropriate flags.
    BumpPtrAllocator Alloc;
    StringSaver SS{Alloc};
    SmallVector<StringRef, 8u> ObjcopyArgs{"llvm-objcopy"};

    for (unsigned I = 0; I < NumberOfInputs; ++I) {
      StringRef InputFile = BundlerConfig.InputFileNames[I];
      if (I == BundlerConfig.HostInputIndex) {
        // Special handling for the host bundle. We do not need to add a
        // standard bundle for the host object since we are going to use fat
        // object as a host object. Therefore use dummy contents (one zero byte)
        // when creating section for the host bundle.
        Expected<StringRef> TempFileOrErr = TempFiles.Create(ArrayRef<char>(0));
        if (!TempFileOrErr)
          return TempFileOrErr.takeError();
        InputFile = *TempFileOrErr;
      }

      ObjcopyArgs.push_back(
          SS.save(Twine("--add-section=") + OFFLOAD_BUNDLER_MAGIC_STR +
                  BundlerConfig.TargetNames[I] + "=" + InputFile));
      ObjcopyArgs.push_back(
          SS.save(Twine("--set-section-flags=") + OFFLOAD_BUNDLER_MAGIC_STR +
                  BundlerConfig.TargetNames[I] + "=readonly,exclude"));
    }
    ObjcopyArgs.push_back("--");
    ObjcopyArgs.push_back(
        BundlerConfig.InputFileNames[BundlerConfig.HostInputIndex]);
    ObjcopyArgs.push_back(BundlerConfig.OutputFileNames.front());

    if (Error Err = executeObjcopy(BundlerConfig.ObjcopyPath, ObjcopyArgs))
      return Err;

    return Error::success();
  }

  Error WriteBundle(raw_fd_ostream &OS, MemoryBuffer &Input) final {
    return Error::success();
  }

private:
  Error executeObjcopy(StringRef Objcopy, ArrayRef<StringRef> Args) {
    // If the user asked for the commands to be printed out, we do that
    // instead of executing it.
    if (BundlerConfig.PrintExternalCommands) {
      errs() << "\"" << Objcopy << "\"";
      for (StringRef Arg : drop_begin(Args, 1))
        errs() << " \"" << Arg << "\"";
      errs() << "\n";
    } else {
      if (sys::ExecuteAndWait(Objcopy, Args))
        return createStringError(inconvertibleErrorCode(),
                                 "'llvm-objcopy' tool failed");
    }
    return Error::success();
  }
};

/// Handler for text files. The bundled file will have the following format.
///
/// "Comment OFFLOAD_BUNDLER_MAGIC_STR__START__ triple"
/// Bundle 1
/// "Comment OFFLOAD_BUNDLER_MAGIC_STR__END__ triple"
/// ...
/// "Comment OFFLOAD_BUNDLER_MAGIC_STR__START__ triple"
/// Bundle N
/// "Comment OFFLOAD_BUNDLER_MAGIC_STR__END__ triple"
class TextFileHandler final : public FileHandler {
  /// String that begins a line comment.
  StringRef Comment;

  /// String that initiates a bundle.
  std::string BundleStartString;

  /// String that closes a bundle.
  std::string BundleEndString;

  /// Number of chars read from input.
  size_t ReadChars = 0u;

protected:
  Error ReadHeader(MemoryBuffer &Input) final { return Error::success(); }

  Expected<std::optional<StringRef>>
  ReadBundleStart(MemoryBuffer &Input) final {
    StringRef FC = Input.getBuffer();

    // Find start of the bundle.
    ReadChars = FC.find(BundleStartString, ReadChars);
    if (ReadChars == FC.npos)
      return std::nullopt;

    // Get position of the triple.
    size_t TripleStart = ReadChars = ReadChars + BundleStartString.size();

    // Get position that closes the triple.
    size_t TripleEnd = ReadChars = FC.find("\n", ReadChars);
    if (TripleEnd == FC.npos)
      return std::nullopt;

    // Next time we read after the new line.
    ++ReadChars;

    return StringRef(&FC.data()[TripleStart], TripleEnd - TripleStart);
  }

  Error ReadBundleEnd(MemoryBuffer &Input) final {
    StringRef FC = Input.getBuffer();

    // Read up to the next new line.
    assert(FC[ReadChars] == '\n' && "The bundle should end with a new line.");

    size_t TripleEnd = ReadChars = FC.find("\n", ReadChars + 1);
    if (TripleEnd != FC.npos)
      // Next time we read after the new line.
      ++ReadChars;

    return Error::success();
  }

  Error ReadBundle(raw_ostream &OS, MemoryBuffer &Input) final {
    StringRef FC = Input.getBuffer();
    size_t BundleStart = ReadChars;

    // Find end of the bundle.
    size_t BundleEnd = ReadChars = FC.find(BundleEndString, ReadChars);

    StringRef Bundle(&FC.data()[BundleStart], BundleEnd - BundleStart);
    OS << Bundle;

    return Error::success();
  }

  Error WriteHeader(raw_fd_ostream &OS,
                    ArrayRef<std::unique_ptr<MemoryBuffer>> Inputs) final {
    return Error::success();
  }

  Error WriteBundleStart(raw_fd_ostream &OS, StringRef TargetTriple) final {
    OS << BundleStartString << TargetTriple << "\n";
    return Error::success();
  }

  Error WriteBundleEnd(raw_fd_ostream &OS, StringRef TargetTriple) final {
    OS << BundleEndString << TargetTriple << "\n";
    return Error::success();
  }

  Error WriteBundle(raw_fd_ostream &OS, MemoryBuffer &Input) final {
    OS << Input.getBuffer();
    return Error::success();
  }

public:
  TextFileHandler(StringRef Comment) : Comment(Comment), ReadChars(0) {
    BundleStartString =
        "\n" + Comment.str() + " " OFFLOAD_BUNDLER_MAGIC_STR "__START__ ";
    BundleEndString =
        "\n" + Comment.str() + " " OFFLOAD_BUNDLER_MAGIC_STR "__END__ ";
  }

  Error listBundleIDsCallback(MemoryBuffer &Input,
                              const BundleInfo &Info) final {
    // TODO: To list bundle IDs in a bundled text file we need to go through
    // all bundles. The format of bundled text file may need to include a
    // header if the performance of listing bundle IDs of bundled text file is
    // important.
    ReadChars = Input.getBuffer().find(BundleEndString, ReadChars);
    if (Error Err = ReadBundleEnd(Input))
      return Err;
    return Error::success();
  }
};
} // namespace

/// Return an appropriate object file handler. We use the specific object
/// handler if we know how to deal with that format, otherwise we use a default
/// binary file handler.
static std::unique_ptr<FileHandler>
CreateObjectFileHandler(MemoryBuffer &FirstInput,
                        const OffloadBundlerConfig &BundlerConfig) {
  // Check if the input file format is one that we know how to deal with.
  Expected<std::unique_ptr<Binary>> BinaryOrErr = createBinary(FirstInput);

  // We only support regular object files. If failed to open the input as a
  // known binary or this is not an object file use the default binary handler.
  if (errorToBool(BinaryOrErr.takeError()) || !isa<ObjectFile>(*BinaryOrErr))
    return std::make_unique<BinaryFileHandler>(BundlerConfig);

  // Otherwise create an object file handler. The handler will be owned by the
  // client of this function.
  return std::make_unique<ObjectFileHandler>(
      std::unique_ptr<ObjectFile>(cast<ObjectFile>(BinaryOrErr->release())),
      BundlerConfig);
}

/// Return an appropriate handler given the input files and options.
static Expected<std::unique_ptr<FileHandler>>
CreateFileHandler(MemoryBuffer &FirstInput,
                  const OffloadBundlerConfig &BundlerConfig) {
  std::string FilesType = BundlerConfig.FilesType;

  if (FilesType == "i")
    return std::make_unique<TextFileHandler>(/*Comment=*/"//");
  if (FilesType == "ii")
    return std::make_unique<TextFileHandler>(/*Comment=*/"//");
  if (FilesType == "cui")
    return std::make_unique<TextFileHandler>(/*Comment=*/"//");
  if (FilesType == "hipi")
    return std::make_unique<TextFileHandler>(/*Comment=*/"//");
  // TODO: `.d` should be eventually removed once `-M` and its variants are
  // handled properly in offload compilation.
  if (FilesType == "d")
    return std::make_unique<TextFileHandler>(/*Comment=*/"#");
  if (FilesType == "ll")
    return std::make_unique<TextFileHandler>(/*Comment=*/";");
#ifdef ENABLE_CLASSIC_FLANG
  if (FilesType == "f95")
    return std::make_unique<TextFileHandler>(/*Comment=*/"!");
#endif
  if (FilesType == "bc")
    return std::make_unique<BinaryFileHandler>(BundlerConfig);
  if (FilesType == "s")
    return std::make_unique<TextFileHandler>(/*Comment=*/"#");
  if (FilesType == "o")
    return CreateObjectFileHandler(FirstInput, BundlerConfig);
  if (FilesType == "a")
    return CreateObjectFileHandler(FirstInput, BundlerConfig);
  if (FilesType == "gch")
    return std::make_unique<BinaryFileHandler>(BundlerConfig);
  if (FilesType == "ast")
    return std::make_unique<BinaryFileHandler>(BundlerConfig);

  return createStringError(errc::invalid_argument,
                           "'" + FilesType + "': invalid file type specified");
}

// List bundle IDs. Return true if an error was found.
Error OffloadBundler::ListBundleIDsInFile(
    StringRef InputFileName, const OffloadBundlerConfig &BundlerConfig) {
  // Open Input file.
  ErrorOr<std::unique_ptr<MemoryBuffer>> CodeOrErr =
      MemoryBuffer::getFileOrSTDIN(InputFileName);
  if (std::error_code EC = CodeOrErr.getError())
    return createFileError(InputFileName, EC);

  MemoryBuffer &Input = **CodeOrErr;

  // Select the right files handler.
  Expected<std::unique_ptr<FileHandler>> FileHandlerOrErr =
      CreateFileHandler(Input, BundlerConfig);
  if (!FileHandlerOrErr)
    return FileHandlerOrErr.takeError();

  std::unique_ptr<FileHandler> &FH = *FileHandlerOrErr;
  assert(FH);
  return FH->listBundleIDs(Input);
}

/// Bundle the files. Return true if an error was found.
Error OffloadBundler::BundleFiles() {
  std::error_code EC;

  // Create output file.
  raw_fd_ostream OutputFile(BundlerConfig.OutputFileNames.front(), EC,
                            sys::fs::OF_None);
  if (EC)
    return createFileError(BundlerConfig.OutputFileNames.front(), EC);

  // Open input files.
  SmallVector<std::unique_ptr<MemoryBuffer>, 8u> InputBuffers;
  InputBuffers.reserve(BundlerConfig.InputFileNames.size());
  for (auto &I : BundlerConfig.InputFileNames) {
    ErrorOr<std::unique_ptr<MemoryBuffer>> CodeOrErr =
        MemoryBuffer::getFileOrSTDIN(I);
    if (std::error_code EC = CodeOrErr.getError())
      return createFileError(I, EC);
    InputBuffers.emplace_back(std::move(*CodeOrErr));
  }

  // Get the file handler. We use the host buffer as reference.
  assert((BundlerConfig.HostInputIndex != ~0u || BundlerConfig.AllowNoHost) &&
         "Host input index undefined??");
  Expected<std::unique_ptr<FileHandler>> FileHandlerOrErr = CreateFileHandler(
      *InputBuffers[BundlerConfig.AllowNoHost ? 0
                                              : BundlerConfig.HostInputIndex],
      BundlerConfig);
  if (!FileHandlerOrErr)
    return FileHandlerOrErr.takeError();

  std::unique_ptr<FileHandler> &FH = *FileHandlerOrErr;
  assert(FH);

  // Write header.
  if (Error Err = FH->WriteHeader(OutputFile, InputBuffers))
    return Err;

  // Write all bundles along with the start/end markers. If an error was found
  // writing the end of the bundle component, abort the bundle writing.
  auto Input = InputBuffers.begin();
  for (auto &Triple : BundlerConfig.TargetNames) {
    if (Error Err = FH->WriteBundleStart(OutputFile, Triple))
      return Err;
    if (Error Err = FH->WriteBundle(OutputFile, **Input))
      return Err;
    if (Error Err = FH->WriteBundleEnd(OutputFile, Triple))
      return Err;
    ++Input;
  }
  return Error::success();
}

// Unbundle the files. Return true if an error was found.
Error OffloadBundler::UnbundleFiles() {
  // Open Input file.
  ErrorOr<std::unique_ptr<MemoryBuffer>> CodeOrErr =
      MemoryBuffer::getFileOrSTDIN(BundlerConfig.InputFileNames.front());
  if (std::error_code EC = CodeOrErr.getError())
    return createFileError(BundlerConfig.InputFileNames.front(), EC);

  MemoryBuffer &Input = **CodeOrErr;

  // Select the right files handler.
  Expected<std::unique_ptr<FileHandler>> FileHandlerOrErr =
      CreateFileHandler(Input, BundlerConfig);
  if (!FileHandlerOrErr)
    return FileHandlerOrErr.takeError();

  std::unique_ptr<FileHandler> &FH = *FileHandlerOrErr;
  assert(FH);

  // Read the header of the bundled file.
  if (Error Err = FH->ReadHeader(Input))
    return Err;

  // Create a work list that consist of the map triple/output file.
  StringMap<StringRef> Worklist;
  auto Output = BundlerConfig.OutputFileNames.begin();
  for (auto &Triple : BundlerConfig.TargetNames) {
    Worklist[Triple] = *Output;
    ++Output;
  }

  // Read all the bundles that are in the work list. If we find no bundles we
  // assume the file is meant for the host target.
  bool FoundHostBundle = false;
  while (!Worklist.empty()) {
    Expected<std::optional<StringRef>> CurTripleOrErr =
        FH->ReadBundleStart(Input);
    if (!CurTripleOrErr)
      return CurTripleOrErr.takeError();

    // We don't have more bundles.
    if (!*CurTripleOrErr)
      break;

    StringRef CurTriple = **CurTripleOrErr;
    assert(!CurTriple.empty());

    auto Output = Worklist.begin();
    for (auto E = Worklist.end(); Output != E; Output++) {
      if (isCodeObjectCompatible(
              OffloadTargetInfo(CurTriple, BundlerConfig),
              OffloadTargetInfo((*Output).first(), BundlerConfig))) {
        break;
      }
    }

    if (Output == Worklist.end())
      continue;
    // Check if the output file can be opened and copy the bundle to it.
    std::error_code EC;
    raw_fd_ostream OutputFile((*Output).second, EC, sys::fs::OF_None);
    if (EC)
      return createFileError((*Output).second, EC);
    if (Error Err = FH->ReadBundle(OutputFile, Input))
      return Err;
    if (Error Err = FH->ReadBundleEnd(Input))
      return Err;
    Worklist.erase(Output);

    // Record if we found the host bundle.
    auto OffloadInfo = OffloadTargetInfo(CurTriple, BundlerConfig);
    if (OffloadInfo.hasHostKind())
      FoundHostBundle = true;
  }

  if (!BundlerConfig.AllowMissingBundles && !Worklist.empty()) {
    std::string ErrMsg = "Can't find bundles for";
    std::set<StringRef> Sorted;
    for (auto &E : Worklist)
      Sorted.insert(E.first());
    unsigned I = 0;
    unsigned Last = Sorted.size() - 1;
    for (auto &E : Sorted) {
      if (I != 0 && Last > 1)
        ErrMsg += ",";
      ErrMsg += " ";
      if (I == Last && I != 0)
        ErrMsg += "and ";
      ErrMsg += E.str();
      ++I;
    }
    return createStringError(inconvertibleErrorCode(), ErrMsg);
  }

  // If no bundles were found, assume the input file is the host bundle and
  // create empty files for the remaining targets.
  if (Worklist.size() == BundlerConfig.TargetNames.size()) {
    for (auto &E : Worklist) {
      std::error_code EC;
      raw_fd_ostream OutputFile(E.second, EC, sys::fs::OF_None);
      if (EC)
        return createFileError(E.second, EC);

      // If this entry has a host kind, copy the input file to the output file.
      auto OffloadInfo = OffloadTargetInfo(E.getKey(), BundlerConfig);
      if (OffloadInfo.hasHostKind())
        OutputFile.write(Input.getBufferStart(), Input.getBufferSize());
    }
    return Error::success();
  }

  // If we found elements, we emit an error if none of those were for the host
  // in case host bundle name was provided in command line.
  if (!(FoundHostBundle || BundlerConfig.HostInputIndex == ~0u ||
        BundlerConfig.AllowMissingBundles))
    return createStringError(inconvertibleErrorCode(),
                             "Can't find bundle for the host target");

  // If we still have any elements in the worklist, create empty files for them.
  for (auto &E : Worklist) {
    std::error_code EC;
    raw_fd_ostream OutputFile(E.second, EC, sys::fs::OF_None);
    if (EC)
      return createFileError(E.second, EC);
  }

  return Error::success();
}

static Archive::Kind getDefaultArchiveKindForHost() {
  return Triple(sys::getDefaultTargetTriple()).isOSDarwin() ? Archive::K_DARWIN
                                                            : Archive::K_GNU;
}

/// @brief Computes a list of targets among all given targets which are
/// compatible with this code object
/// @param [in] CodeObjectInfo Code Object
/// @param [out] CompatibleTargets List of all compatible targets among all
/// given targets
/// @return false, if no compatible target is found.
static bool
getCompatibleOffloadTargets(OffloadTargetInfo &CodeObjectInfo,
                            SmallVectorImpl<StringRef> &CompatibleTargets,
                            const OffloadBundlerConfig &BundlerConfig) {
  if (!CompatibleTargets.empty()) {
    DEBUG_WITH_TYPE("CodeObjectCompatibility",
                    dbgs() << "CompatibleTargets list should be empty\n");
    return false;
  }
  for (auto &Target : BundlerConfig.TargetNames) {
    auto TargetInfo = OffloadTargetInfo(Target, BundlerConfig);
    if (isCodeObjectCompatible(CodeObjectInfo, TargetInfo))
      CompatibleTargets.push_back(Target);
  }
  return !CompatibleTargets.empty();
}

/// UnbundleArchive takes an archive file (".a") as input containing bundled
/// code object files, and a list of offload targets (not host), and extracts
/// the code objects into a new archive file for each offload target. Each
/// resulting archive file contains all code object files corresponding to that
/// particular offload target. The created archive file does not
/// contain an index of the symbols and code object files are named as
/// <<Parent Bundle Name>-<CodeObject's GPUArch>>, with ':' replaced with '_'.
Error OffloadBundler::UnbundleArchive() {
  std::vector<std::unique_ptr<MemoryBuffer>> ArchiveBuffers;

  /// Map of target names with list of object files that will form the device
  /// specific archive for that target
  StringMap<std::vector<NewArchiveMember>> OutputArchivesMap;

  // Map of target names and output archive filenames
  StringMap<StringRef> TargetOutputFileNameMap;

  auto Output = BundlerConfig.OutputFileNames.begin();
  for (auto &Target : BundlerConfig.TargetNames) {
    TargetOutputFileNameMap[Target] = *Output;
    ++Output;
  }

  StringRef IFName = BundlerConfig.InputFileNames.front();

  ErrorOr<std::unique_ptr<MemoryBuffer>> BufOrErr =
      MemoryBuffer::getFileOrSTDIN(IFName, true, false);
  if (std::error_code EC = BufOrErr.getError())
    return createFileError(BundlerConfig.InputFileNames.front(), EC);

  ArchiveBuffers.push_back(std::move(*BufOrErr));
  Expected<std::unique_ptr<llvm::object::Archive>> LibOrErr =
      Archive::create(ArchiveBuffers.back()->getMemBufferRef());
  if (!LibOrErr)
    return LibOrErr.takeError();

  auto Archive = std::move(*LibOrErr);

  Error ArchiveErr = Error::success();
  auto ChildEnd = Archive->child_end();

  /// Iterate over all bundled code object files in the input archive.
  for (auto ArchiveIter = Archive->child_begin(ArchiveErr);
       ArchiveIter != ChildEnd; ++ArchiveIter) {
    if (ArchiveErr)
      return ArchiveErr;
    auto ArchiveChildNameOrErr = (*ArchiveIter).getName();
    if (!ArchiveChildNameOrErr)
      return ArchiveChildNameOrErr.takeError();

    StringRef BundledObjectFile = sys::path::filename(*ArchiveChildNameOrErr);

    auto CodeObjectBufferRefOrErr = (*ArchiveIter).getMemoryBufferRef();
    if (!CodeObjectBufferRefOrErr)
      return CodeObjectBufferRefOrErr.takeError();

    auto CodeObjectBuffer =
        MemoryBuffer::getMemBuffer(*CodeObjectBufferRefOrErr, false);

    Expected<std::unique_ptr<FileHandler>> FileHandlerOrErr =
        CreateFileHandler(*CodeObjectBuffer, BundlerConfig);
    if (!FileHandlerOrErr)
      return FileHandlerOrErr.takeError();

    std::unique_ptr<FileHandler> &FileHandler = *FileHandlerOrErr;
    assert(FileHandler &&
           "FileHandle creation failed for file in the archive!");

    if (Error ReadErr = FileHandler->ReadHeader(*CodeObjectBuffer))
      return ReadErr;

    Expected<std::optional<StringRef>> CurBundleIDOrErr =
        FileHandler->ReadBundleStart(*CodeObjectBuffer);
    if (!CurBundleIDOrErr)
      return CurBundleIDOrErr.takeError();

    std::optional<StringRef> OptionalCurBundleID = *CurBundleIDOrErr;
    // No device code in this child, skip.
    if (!OptionalCurBundleID)
      continue;
    StringRef CodeObject = *OptionalCurBundleID;

    // Process all bundle entries (CodeObjects) found in this child of input
    // archive.
    while (!CodeObject.empty()) {
      SmallVector<StringRef> CompatibleTargets;
      auto CodeObjectInfo = OffloadTargetInfo(CodeObject, BundlerConfig);
      if (CodeObjectInfo.hasHostKind()) {
        // Do nothing, we don't extract host code yet.
      } else if (getCompatibleOffloadTargets(CodeObjectInfo, CompatibleTargets,
                                             BundlerConfig)) {
        std::string BundleData;
        raw_string_ostream DataStream(BundleData);
        if (Error Err = FileHandler->ReadBundle(DataStream, *CodeObjectBuffer))
          return Err;

        for (auto &CompatibleTarget : CompatibleTargets) {
          SmallString<128> BundledObjectFileName;
          BundledObjectFileName.assign(BundledObjectFile);
          auto OutputBundleName =
              Twine(llvm::sys::path::stem(BundledObjectFileName) + "-" +
                    CodeObject +
                    getDeviceLibraryFileName(BundledObjectFileName,
                                             CodeObjectInfo.TargetID))
                  .str();
          // Replace ':' in optional target feature list with '_' to ensure
          // cross-platform validity.
          std::replace(OutputBundleName.begin(), OutputBundleName.end(), ':',
                       '_');

          std::unique_ptr<MemoryBuffer> MemBuf = MemoryBuffer::getMemBufferCopy(
              DataStream.str(), OutputBundleName);
          ArchiveBuffers.push_back(std::move(MemBuf));
          llvm::MemoryBufferRef MemBufRef =
              MemoryBufferRef(*(ArchiveBuffers.back()));

          // For inserting <CompatibleTarget, list<CodeObject>> entry in
          // OutputArchivesMap.
          if (!OutputArchivesMap.contains(CompatibleTarget)) {

            std::vector<NewArchiveMember> ArchiveMembers;
            ArchiveMembers.push_back(NewArchiveMember(MemBufRef));
            OutputArchivesMap.insert_or_assign(CompatibleTarget,
                                               std::move(ArchiveMembers));
          } else {
            OutputArchivesMap[CompatibleTarget].push_back(
                NewArchiveMember(MemBufRef));
          }
        }
      }

      if (Error Err = FileHandler->ReadBundleEnd(*CodeObjectBuffer))
        return Err;

      Expected<std::optional<StringRef>> NextTripleOrErr =
          FileHandler->ReadBundleStart(*CodeObjectBuffer);
      if (!NextTripleOrErr)
        return NextTripleOrErr.takeError();

      CodeObject = ((*NextTripleOrErr).has_value()) ? **NextTripleOrErr : "";
    } // End of processing of all bundle entries of this child of input archive.
  }   // End of while over children of input archive.

  assert(!ArchiveErr && "Error occurred while reading archive!");

  /// Write out an archive for each target
  for (auto &Target : BundlerConfig.TargetNames) {
    StringRef FileName = TargetOutputFileNameMap[Target];
    StringMapIterator<std::vector<llvm::NewArchiveMember>> CurArchiveMembers =
        OutputArchivesMap.find(Target);
    if (CurArchiveMembers != OutputArchivesMap.end()) {
      if (Error WriteErr = writeArchive(FileName, CurArchiveMembers->getValue(),
                                        true, getDefaultArchiveKindForHost(),
                                        true, false, nullptr))
        return WriteErr;
    } else if (!BundlerConfig.AllowMissingBundles) {
      std::string ErrMsg =
          Twine("no compatible code object found for the target '" + Target +
                "' in heterogeneous archive library: " + IFName)
              .str();
      return createStringError(inconvertibleErrorCode(), ErrMsg);
    } else { // Create an empty archive file if no compatible code object is
             // found and "allow-missing-bundles" is enabled. It ensures that
             // the linker using output of this step doesn't complain about
             // the missing input file.
      std::vector<llvm::NewArchiveMember> EmptyArchive;
      EmptyArchive.clear();
      if (Error WriteErr = writeArchive(FileName, EmptyArchive, true,
                                        getDefaultArchiveKindForHost(), true,
                                        false, nullptr))
        return WriteErr;
    }
  }

  return Error::success();
}
