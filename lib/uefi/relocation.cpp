
#include "relocation.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "pe.h"

int relocate_image(char *image) {
  const auto dos_header = reinterpret_cast<IMAGE_DOS_HEADER *>(image);
  const auto pe_header = dos_header->GetPEHeader();
  const auto optional_header = &pe_header->OptionalHeader;
  const auto Adjust =
      reinterpret_cast<size_t>(image - optional_header->ImageBase);

  // DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC] is only valid when the
  // optional header both declares (NumberOfRvaAndSizes) and physically contains
  // (SizeOfOptionalHeader) that entry; otherwise indexing it would read
  // section-table bytes past the header and mis-parse them.
  constexpr size_t kRelocDirEnd =
      offsetof(IMAGE_OPTIONAL_HEADER64, DataDirectory) +
      (IMAGE_DIRECTORY_ENTRY_BASERELOC + 1) * sizeof(IMAGE_DATA_DIRECTORY);
  const bool have_reloc_dir =
      optional_header->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC &&
      pe_header->FileHeader.SizeOfOptionalHeader >= kRelocDirEnd;
  const auto reloc_directory =
      have_reloc_dir
          ? optional_header->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC]
          : IMAGE_DATA_DIRECTORY{};
  if (!have_reloc_dir || reloc_directory.Size == 0) {
    // No relocations to apply. That is safe only if the image already sits at
    // its preferred base or is position independent. An image whose relocations
    // were stripped (IMAGE_FILE_RELOCS_STRIPPED: it must load at ImageBase) that
    // landed elsewhere would run with unadjusted absolute addresses, so reject
    // it instead of reporting success.
    if (Adjust != 0 &&
        (pe_header->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED)) {
      printf("Image relocations stripped but not loaded at its ImageBase\n");
      return -1;
    }
    printf("%s\n", have_reloc_dir ? "Relocation section empty"
                                  : "No base relocation directory present");
    return 0;
  }
  auto RelocBase = reinterpret_cast<EFI_IMAGE_BASE_RELOCATION *>(
      image + reloc_directory.VirtualAddress);
  const auto RelocBaseEnd = reinterpret_cast<EFI_IMAGE_BASE_RELOCATION *>(
      reinterpret_cast<char *>(RelocBase) + reloc_directory.Size);
  //
  // Run this relocation record
  //
  while (RelocBase < RelocBaseEnd) {
    auto Reloc =
        reinterpret_cast<uint16_t *>(reinterpret_cast<char *>(RelocBase) +
                                     sizeof(EFI_IMAGE_BASE_RELOCATION));
    auto RelocEnd = reinterpret_cast<uint16_t *>(
        reinterpret_cast<char *>(RelocBase) + RelocBase->SizeOfBlock);
    if (RelocBase->SizeOfBlock == 0) {
      printf("Found relocation block of size 0, this is wrong\n");
      return -1;
    }
    while (Reloc < RelocEnd) {
      auto Fixup = image + RelocBase->VirtualAddress + (*Reloc & 0xFFF);
      if (Fixup == nullptr) {
        return 0;
      }

      auto Fixup16 = reinterpret_cast<uint16_t *>(Fixup);
      auto Fixup32 = reinterpret_cast<uint32_t *>(Fixup);
      auto Fixup64 = reinterpret_cast<uint64_t *>(Fixup);
      switch ((*Reloc) >> 12) {
      case EFI_IMAGE_REL_BASED_ABSOLUTE:
        break;

      case EFI_IMAGE_REL_BASED_HIGH:
        *Fixup16 = static_cast<uint16_t>(
            *Fixup16 +
            (static_cast<uint16_t>(static_cast<uint32_t>(Adjust) >> 16)));

        break;

      case EFI_IMAGE_REL_BASED_LOW:
        *Fixup16 = static_cast<uint16_t>(
            *Fixup16 + (static_cast<uint16_t>(Adjust) & 0xffff));

        break;

      case EFI_IMAGE_REL_BASED_HIGHLOW:
        *Fixup32 = *Fixup32 + static_cast<uint32_t>(Adjust);
        break;

      case EFI_IMAGE_REL_BASED_DIR64:
        *Fixup64 = *Fixup64 + static_cast<uint64_t>(Adjust);
        break;

      case EFI_IMAGE_REL_BASED_ARM_MOV32A:
        // ARM MOVW/MOVT instruction encoding is not implemented. Reject the
        // image rather than letting it reach its entry point with this
        // relocation left unapplied.
        printf("Unsupported relocation type: EFI_IMAGE_REL_BASED_ARM_MOV32A\n");
        return -1;
      case EFI_IMAGE_REL_BASED_LOONGARCH64_MARK_LA: {
        // The next four instructions are used to load a 64 bit address,
        // relocate all of them
        if (reinterpret_cast<char *>(Fixup32) + 4 * sizeof(uint32_t) > image + optional_header->SizeOfImage) {
          printf("Relocation out of bounds\n");
          return -1;
        }

        uint64_t Value =
            (*(Fixup32 + 0) & 0x1ffffe0) << 7 |           // lu12i.w 20bits from bit5
            (*(Fixup32 + 1) & 0x3ffc00) >> 10;      // ori     12bits from bit10
        uint64_t Tmp1 = *(Fixup32 + 2) & 0x1ffffe0; // lu32i.d 20bits from bit5
        uint64_t Tmp2 = *(Fixup32 + 3) & 0x3ffc00;  // lu52i.d 12bits from bit10
        Value = Value | (Tmp1 << 27) | (Tmp2 << 42);
        Value += Adjust;

        *(Fixup32 + 0) = (*(Fixup32 + 0) & ~0x1ffffe0) | (((Value >> 12) & 0xfffff) << 5);
        *(Fixup32 + 1) = (*(Fixup32 + 1) & ~0x3ffc00) | ((Value & 0xfff) << 10);
        *(Fixup32 + 2) = (*(Fixup32 + 2) & ~0x1ffffe0) | (((Value >> 32) & 0xfffff) << 5);
        *(Fixup32 + 3) = (*(Fixup32 + 3) & ~0x3ffc00) | (((Value >> 52) & 0xfff) << 10);
        break;
      }
      default:
        printf("Unsupported relocation type: %d\n", (*Reloc) >> 12);
        return -1;
      }

      //
      // Next relocation record
      //
      Reloc += 1;
    }
    RelocBase = reinterpret_cast<EFI_IMAGE_BASE_RELOCATION *>(RelocEnd);
  }
  optional_header->ImageBase = reinterpret_cast<size_t>(image);
  return 0;
}
