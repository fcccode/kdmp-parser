// Axel '0vercl0k' Souchet - February 15 2019
#include "kdmp-parser.h"

KernelDumpParser::KernelDumpParser(const TCHAR *PathFile)
    : File_(nullptr), FileMap_(nullptr), ViewBase_(nullptr), DmpHdr_(nullptr),
      PathFile_(PathFile) {}

KernelDumpParser::~KernelDumpParser() {

  //
  // Empty out the physmem.
  //

  Physmem_.clear();

  //
  // Unmap the view of the mapping..
  //

  if (ViewBase_ != nullptr) {
    UnmapViewOfFile(ViewBase_);
    ViewBase_ = nullptr;
  }

  //
  // Close the handle to the file mapping..
  //

  if (FileMap_ != nullptr) {
    CloseHandle(FileMap_);
    FileMap_ = nullptr;
  }

  //
  // And finally the file itself.
  //

  if (File_ != nullptr) {
    CloseHandle(File_);
    File_ = nullptr;
  }
}

bool KernelDumpParser::Parse() {

  //
  // Map a view of the file.
  //

  if (!MapFile()) {
    _tprintf(_T("MapFile failed.\n"));
    return false;
  }

  //
  // Parse the DMP_HEADER.
  //

  if (!ParseDmpHeader()) {
    _tprintf(_T("ParseDmpHeader failed.\n"));
    return false;
  }

  //
  // Retrieve the physical memory according to the type of dump we have.
  //

  if (DmpHdr_->DumpType == FullDump) {
    if (!BuildPhysmemFullDump()) {
      _tprintf(_T("BuildPhysmemFullDump failed.\n"));
      return false;
    }
  } else if (DmpHdr_->DumpType == BMPDump) {
    if (!BuildPhysmemBMPDump()) {
      _tprintf(_T("BuildPhysmemBMPDump failed.\n"));
      return false;
    }
  }

  return true;
}

bool KernelDumpParser::ParseDmpHeader() {

  //
  // The base of the view points on the DMP_HEADER64.
  //

  DmpHdr_ = (KDMP_PARSER_HEADER64 *)ViewBase_;

  //
  // Now let's make sure the structures look right.
  //

  if (!DmpHdr_->LooksGood()) {
    _tprintf(_T("The header looks wrong.\n"));
    return false;
  }

  return true;
}

const KDMP_PARSER_CONTEXT *KernelDumpParser::GetContext() {

  //
  // Give the user a view of the context record.
  //

  return &DmpHdr_->ContextRecord;
}

bool KernelDumpParser::MapFile() {
  bool Success = true;
  HANDLE File = nullptr;
  HANDLE FileMap = nullptr;
  PVOID ViewBase = nullptr;

  //
  // Open the dump file in read-only.
  //

  File = CreateFile(PathFile_, GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, 0, nullptr);

  if (File == NULL) {

    //
    // If we fail to open the file, let the user know.
    //

    const DWORD GLE = GetLastError();
    _tprintf(_T("CreateFile failed with GLE=%d.\n"), GLE);

    if (GLE == ERROR_FILE_NOT_FOUND) {
      _tprintf(_T("  The file %s was not found.\n"), PathFile_);
    }

    Success = false;
    goto clean;
  }

  //
  // Create the ro file mapping.
  //

  FileMap = CreateFileMapping(File, nullptr, PAGE_READONLY, 0, 0,
                              _T("Kernel crash-dump."));

  if (FileMap == nullptr) {

    //
    // If we fail to create a file mapping, let
    // the user know.
    //

    const DWORD GLE = GetLastError();
    _tprintf(_T("CreateFileMapping failed with GLE=%d.\n"), GLE);
    Success = false;
    goto clean;
  }

  //
  // Map a view of the file in memory.
  //

  ViewBase = MapViewOfFile(FileMap, FILE_MAP_READ, 0, 0, 0);

  if (ViewBase == nullptr) {

    //
    // If we fail to map the view, let the user know.
    //

    const DWORD GLE = GetLastError();
    _tprintf(_T("MapViewOfFile failed with GLE=%d.\n"), GLE);
    Success = false;
    goto clean;
  }

  //
  // Everything went well, so grab a copy of the handles for
  // our class and null-out the temporary variables.
  //

  File_ = File;
  File = nullptr;

  FileMap_ = FileMap;
  FileMap = nullptr;

  ViewBase_ = ViewBase;
  ViewBase = nullptr;

clean:

  //
  // Unmap the view of the mapping..
  //

  if (ViewBase != nullptr) {
    UnmapViewOfFile(ViewBase);
    ViewBase = nullptr;
  }

  //
  // Close the handle to the file mapping..
  //

  if (FileMap != nullptr) {
    CloseHandle(FileMap);
    FileMap = nullptr;
  }

  //
  // And finally the file itself.
  //

  if (File != nullptr) {
    CloseHandle(File);
    File = nullptr;
  }

  return Success;
}

bool KernelDumpParser::BuildPhysmemBMPDump() {

  const uint8_t *Page = (uint8_t *)DmpHdr_ + DmpHdr_->BmpHeader.FirstPage;
  const uint64_t BitmapSize = DmpHdr_->BmpHeader.Pages / 8;
  const uint8_t *Bitmap = DmpHdr_->BmpHeader.Bitmap;

  //
  // Walk the bitmap byte per byte.
  //

  for (uint64_t BitmapIdx = 0; BitmapIdx < BitmapSize; BitmapIdx++) {

    //
    // Now walk the bits of the current byte.
    //

    const uint8_t Byte = Bitmap[BitmapIdx];
    for (uint8_t BitIdx = 0; BitIdx < 8; BitIdx++) {

      //
      // If the bit is not set we just skip to the next.
      //

      const bool BitSet = ((Byte >> BitIdx) & 1) == 1;
      if (!BitSet) {
        continue;
      }

      //
      // If the bit is one we add the page to the physmem.
      //

      const uint64_t Pfn = (BitmapIdx * 8) + BitIdx;
      const uint64_t Pa = Pfn * 0x1000;
      Physmem_.try_emplace(Pa, Page);
      Page += 0x1000;
    }
  }

  return true;
}

bool KernelDumpParser::BuildPhysmemFullDump() {

  //
  // Walk through the runs.
  //

  uint8_t *RunBase = (uint8_t *)&DmpHdr_->BmpHeader;
  const uint32_t NumberOfRuns = DmpHdr_->PhysicalMemoryBlockBuffer.NumberOfRuns;

  //
  // Back at it, this time building the index!
  //

  for (uint32_t RunIdx = 0; RunIdx < NumberOfRuns; RunIdx++) {

    //
    // Grab the current run as well as its base page and page count.
    //

    const KDMP_PARSER_PHYSMEM_RUN *Run =
        &DmpHdr_->PhysicalMemoryBlockBuffer.Run[RunIdx];

    const uint64_t BasePage = Run->BasePage;
    const uint64_t PageCount = Run->PageCount;

    //
    // Walk the pages from the run.
    //

    for (uint32_t PageIdx = 0; PageIdx < PageCount; PageIdx++) {

      //
      // Compute the current PFN as well as the actual physical address of the
      // page.
      //

      const uint64_t Pfn = BasePage + PageIdx;
      const uint64_t Pa = Pfn * 0x1000;

      //
      // Now one thing to understand is that the Runs structure allows to skip
      // for holes in memory. Instead of, padding them with empty spaces to
      // conserve a 1:1 mapping between physical address and file offset, the
      // Run gives you the base Pfn. This means that we don't have a 1:1 mapping
      // between file offset and physical addresses so we need to keep track of
      // where the Run starts in memory and then we can simply access our pages
      // one after the other.
      //
      // If this is not clear enough, here is a small example:
      //  Run[0]
      //    BasePage = 1337, PageCount = 2
      //  Run[1]
      //    BasePage = 1400, PageCount = 1
      //
      // In the above we clearly see that there is a hole between the two runs;
      // the dump file has 2+1 memory pages. Their Pfns are: 1337+0, 1337+1,
      // 1400+0.
      //
      // Now if we want to get the file offset of those pages we start at Run0:
      //   Run0 starts at file offset 0x2000 so Page0 is at file offset 0x2000,
      //   Page1 is at file offset 0x3000. Run1 starts at file offset
      //   0x2000+(2*0x1000) so Page3 is at file offset
      //   0x2000+(2*0x1000)+0x1000.
      //
      // That is the reason why the computation below is RunBase + (PageIdx *
      // 0x1000) instead of RunBase + (Pfn * 0x1000).

      const uint8_t *PageBase = RunBase + (uint64_t(PageIdx) * 0x1000);

      //
      // Map the Pfn to a page.
      //

      Physmem_.try_emplace(Pa, PageBase);
    }

    //
    // Move the run base past all the pages in the current run.
    //

    RunBase += PageCount * 0x1000;
  }

  return true;
}

const Physmem_t &KernelDumpParser::GetPhysmem() { return Physmem_; }

void KernelDumpParser::ShowContextRecord(const uint32_t Prefix = 0) const {
  const KDMP_PARSER_CONTEXT &Context = DmpHdr_->ContextRecord;
  _tprintf(_T("%*srax=%016llx rbx=%016llx rcx=%016llx\n"), Prefix, _T(""),
           Context.Rax, Context.Rbx, Context.Rcx);
  _tprintf(_T("%*srdx=%016llx rsi=%016llx rdi=%016llx\n"), Prefix, _T(""),
           Context.Rdx, Context.Rsi, Context.Rdi);
  _tprintf(_T("%*srip=%016llx rsp=%016llx rbp=%016llx\n"), Prefix, _T(""),
           Context.Rip, Context.Rsp, Context.Rbp);
  _tprintf(_T("%*s r8=%016llx  r9=%016llx r10=%016llx\n"), Prefix, _T(""),
           Context.R8, Context.R9, Context.R10);
  _tprintf(_T("%*sr11=%016llx r12=%016llx r13=%016llx\n"), Prefix, _T(""),
           Context.R11, Context.R12, Context.R13);
  _tprintf(_T("%*sr14=%016llx r15=%016llx\n"), Prefix, _T(""), Context.R14,
           Context.R15);
  _tprintf(_T("%*scs=%04x ss=%04x ds=%04x es=%04x fs=%04x gs=%04x    ")
           _T("             efl=%08x\n"),
           Prefix, _T(""), Context.SegCs, Context.SegSs, Context.SegDs,
           Context.SegEs, Context.SegFs, Context.SegGs, Context.EFlags);
  _tprintf(_T("%*sfpcw=%04x    fpsw=%04x    fptw=%04x\n"), Prefix, _T(""),
           Context.ControlWord, Context.StatusWord, 1);
  _tprintf(_T("%*s  st0=%016llx%016llx       st1=%016llx%016llx\n"), Prefix,
           _T(""), Context.FloatRegisters[0].High,
           Context.FloatRegisters[0].Low, Context.FloatRegisters[1].High,
           Context.FloatRegisters[1].Low);
  _tprintf(_T("%*s  st2=%016llx%016llx       st3=%016llx%016llx\n"), Prefix,
           _T(""), Context.FloatRegisters[2].High,
           Context.FloatRegisters[2].Low, Context.FloatRegisters[3].High,
           Context.FloatRegisters[3].Low);
  _tprintf(_T("%*s  st4=%016llx%016llx       st5=%016llx%016llx\n"), Prefix,
           _T(""), Context.FloatRegisters[4].High,
           Context.FloatRegisters[4].Low, Context.FloatRegisters[5].High,
           Context.FloatRegisters[5].Low);
  _tprintf(_T("%*s  st6=%016llx%016llx       st7=%016llx%016llx\n"), Prefix,
           _T(""), Context.FloatRegisters[6].High,
           Context.FloatRegisters[6].Low, Context.FloatRegisters[7].High,
           Context.FloatRegisters[7].Low);
  _tprintf(_T("%*s xmm0=%016llx%016llx      xmm1=%016llx%016llx\n"), Prefix,
           _T(""), Context.Xmm0.High, Context.Xmm0.Low, Context.Xmm1.High,
           Context.Xmm1.Low);
  _tprintf(_T("%*s xmm2=%016llx%016llx      xmm3=%016llx%016llx\n"), Prefix,
           _T(""), Context.Xmm2.High, Context.Xmm2.Low, Context.Xmm3.High,
           Context.Xmm3.Low);
  _tprintf(_T("%*s xmm4=%016llx%016llx      xmm5=%016llx%016llx\n"), Prefix,
           _T(""), Context.Xmm4.High, Context.Xmm4.Low, Context.Xmm5.High,
           Context.Xmm5.Low);
  _tprintf(_T("%*s xmm6=%016llx%016llx      xmm7=%016llx%016llx\n"), Prefix,
           _T(""), Context.Xmm6.High, Context.Xmm6.Low, Context.Xmm7.High,
           Context.Xmm7.Low);
  _tprintf(_T("%*s xmm8=%016llx%016llx      xmm9=%016llx%016llx\n"), Prefix,
           _T(""), Context.Xmm8.High, Context.Xmm8.Low, Context.Xmm9.High,
           Context.Xmm9.Low);
  _tprintf(_T("%*sxmm10=%016llx%016llx     xmm11=%016llx%016llx\n"), Prefix,
           _T(""), Context.Xmm10.High, Context.Xmm10.Low, Context.Xmm11.High,
           Context.Xmm11.Low);
  _tprintf(_T("%*sxmm12=%016llx%016llx     xmm13=%016llx%016llx\n"), Prefix,
           _T(""), Context.Xmm12.High, Context.Xmm12.Low, Context.Xmm13.High,
           Context.Xmm13.Low);
  _tprintf(_T("%*sxmm14=%016llx%016llx     xmm15=%016llx%016llx\n"), Prefix,
           _T(""), Context.Xmm14.High, Context.Xmm14.Low, Context.Xmm15.High,
           Context.Xmm15.Low);
}

void KernelDumpParser::ShowExceptionRecord(const uint32_t Prefix = 0) const {
  DmpHdr_->Exception.Show(Prefix);
}

void KernelDumpParser::ShowAllStructures(const uint32_t Prefix = 0) const {
  DmpHdr_->Show(Prefix);
}

const uint8_t *
KernelDumpParser::GetPhysicalPage(const uint64_t PhysicalAddress) const {

  //
  // Attempt to find the physical address.
  //

  Physmem_t::const_iterator Pair = Physmem_.find(PhysicalAddress);

  //
  // If it doesn't exist then return nullptr.
  //

  if (Pair == Physmem_.end()) {
    return nullptr;
  }

  //
  // Otherwise we return a pointer to the content of the page.
  //

  return Pair->second;
}
