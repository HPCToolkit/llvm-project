//===- Relocations.h --------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLD_MACHO_RELOCATIONS_H
#define LLD_MACHO_RELOCATIONS_H

#include "llvm/ADT/BitmaskEnum.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/BinaryFormat/MachO.h"

#include <cstddef>
#include <cstdint>

namespace lld {
namespace macho {
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

class Symbol;
class InputSection;

enum class RelocAttrBits {
  _0 = 0,              // invalid
  PCREL = 1 << 0,      // Value is PC-relative offset
  ABSOLUTE = 1 << 1,   // Value is an absolute address or fixed offset
  BYTE4 = 1 << 2,      // 4 byte datum
  BYTE8 = 1 << 3,      // 8 byte datum
  EXTERN = 1 << 4,     // Can have an external symbol
  LOCAL = 1 << 5,      // Can have a local symbol
  ADDEND = 1 << 6,     // *_ADDEND paired prefix reloc
  SUBTRAHEND = 1 << 7, // *_SUBTRACTOR paired prefix reloc
  BRANCH = 1 << 8,     // Value is branch target
  GOT = 1 << 9,        // References a symbol in the Global Offset Table
  TLV = 1 << 10,       // References a thread-local symbol
  DYSYM8 = 1 << 11,    // Requires DySym width to be 8 bytes
  LOAD = 1 << 12,      // Relaxable indirect load
  POINTER = 1 << 13,   // Non-relaxable indirect load (pointer is taken)
  UNSIGNED = 1 << 14,  // *_UNSIGNED relocs
  LLVM_MARK_AS_BITMASK_ENUM(/*LargestValue*/ (1 << 15) - 1),
};
// Note: SUBTRACTOR always pairs with UNSIGNED (a delta between two symbols).

struct RelocAttrs {
  llvm::StringRef name;
  RelocAttrBits bits;
  bool hasAttr(RelocAttrBits b) const { return (bits & b) == b; }
};

struct Reloc {
  uint8_t type = llvm::MachO::GENERIC_RELOC_INVALID;
  bool pcrel = false;
  uint8_t length = 0;
  // The offset from the start of the subsection that this relocation belongs
  // to.
  uint32_t offset = 0;
  // Adding this offset to the address of the referent symbol or subsection
  // gives the destination that this relocation refers to.
  uint64_t addend = 0;
  llvm::PointerUnion<Symbol *, InputSection *> referent = nullptr;
};

bool validateSymbolRelocation(const Symbol *, const InputSection *,
                              const Reloc &);

extern const RelocAttrs invalidRelocAttrs;

} // namespace macho
} // namespace lld

#endif
