/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "hermes/VM/StringPrimitive.h"

#include "hermes/Support/Algorithms.h"
#include "hermes/Support/UTF8.h"
#include "hermes/VM/BuildMetadata.h"
#include "hermes/VM/FillerCell.h"
#include "hermes/VM/StringBuilder.h"
#include "hermes/VM/StringView.h"

namespace hermes {
namespace vm {

// Has nothing to do, just here to stop linker errors.
void DynamicASCIIStringPrimitiveBuildMeta(
    const GCCell *cell,
    Metadata::Builder &mb) {}
void DynamicUTF16StringPrimitiveBuildMeta(
    const GCCell *cell,
    Metadata::Builder &mb) {}

/// There is no SymbolStringPrimitiveCellKind, but we factor this into a
/// function so that the subclasses can share it and so only one friend
/// declaration is required.
void symbolStringPrimitiveBuildMeta(const GCCell *cell, Metadata::Builder &mb) {
  const auto *self = static_cast<const SymbolStringPrimitive *>(cell);
  mb.addField("uniqueID", &self->uniqueID_);
}

void DynamicUniquedASCIIStringPrimitiveBuildMeta(
    const GCCell *cell,
    Metadata::Builder &mb) {
  symbolStringPrimitiveBuildMeta(cell, mb);
}

void DynamicUniquedUTF16StringPrimitiveBuildMeta(
    const GCCell *cell,
    Metadata::Builder &mb) {
  symbolStringPrimitiveBuildMeta(cell, mb);
}

void ExternalASCIIStringPrimitiveBuildMeta(
    const GCCell *cell,
    Metadata::Builder &mb) {
  symbolStringPrimitiveBuildMeta(cell, mb);
}

void ExternalUTF16StringPrimitiveBuildMeta(
    const GCCell *cell,
    Metadata::Builder &mb) {
  symbolStringPrimitiveBuildMeta(cell, mb);
}

template <typename T>
CallResult<HermesValue> StringPrimitive::createEfficientImpl(
    Runtime *runtime,
    llvm::ArrayRef<T> str,
    std::basic_string<T> *optStorage) {
  constexpr bool charIs8Bit = std::is_same<T, char>::value;
  assert(
      (!optStorage ||
       str == llvm::makeArrayRef(optStorage->data(), optStorage->size())) &&
      "If optStorage is provided, it must equal the input string");
  assert(
      (!charIs8Bit || isAllASCII(str.begin(), str.end())) &&
      "8 bit strings must be ASCII");
  if (str.empty()) {
    return HermesValue::encodeStringValue(
        runtime->getPredefinedString(Predefined::emptyString));
  }
  if (str.size() == 1) {
    return runtime->getCharacterString(str[0]).getHermesValue();
  }

  // Check if we should acquire ownership of storage.
  if (optStorage != nullptr &&
      str.size() >= StringPrimitive::EXTERNAL_STRING_MIN_SIZE) {
    return ExternalStringPrimitive<T>::create(runtime, std::move(*optStorage));
  }

  // Check if we fit in ASCII.
  // We are ASCII if we are 8 bit, or we are 16 bit and all of our text is
  // ASCII.
  bool isAscii = charIs8Bit || isAllASCII(str.begin(), str.end());
  if (isAscii) {
    auto result = StringPrimitive::create(runtime, str.size(), isAscii);
    if (LLVM_UNLIKELY(result == ExecutionStatus::EXCEPTION)) {
      return ExecutionStatus::EXCEPTION;
    }
    auto output = runtime->makeHandle<StringPrimitive>(*result);
    // Copy directly into the StringPrimitive storage.
    std::copy(str.begin(), str.end(), output->castToASCIIPointerForWrite());
    return output.getHermesValue();
  }

  return StringPrimitive::create(runtime, str);
}

CallResult<HermesValue> StringPrimitive::createEfficient(
    Runtime *runtime,
    ASCIIRef str) {
  return createEfficientImpl(runtime, str);
}

CallResult<HermesValue> StringPrimitive::createEfficient(
    Runtime *runtime,
    UTF16Ref str) {
  return createEfficientImpl(runtime, str);
}

CallResult<HermesValue> StringPrimitive::createEfficient(
    Runtime *runtime,
    std::basic_string<char> &&str) {
  return createEfficientImpl(
      runtime, llvm::makeArrayRef(str.data(), str.size()), &str);
}

CallResult<HermesValue> StringPrimitive::createEfficient(
    Runtime *runtime,
    std::basic_string<char16_t> &&str) {
  return createEfficientImpl(
      runtime, llvm::makeArrayRef(str.data(), str.size()), &str);
}

bool StringPrimitive::sliceEquals(
    uint32_t start,
    uint32_t length,
    const StringPrimitive *other) const {
  if (isASCII()) {
    if (other->isASCII()) {
      return stringRefEquals(
          castToASCIIRef(start, length), other->castToASCIIRef());
    }
    return stringRefEquals(
        castToASCIIRef(start, length), other->castToUTF16Ref());
  }
  if (other->isASCII()) {
    return stringRefEquals(
        castToUTF16Ref(start, length), other->castToASCIIRef());
  }
  return stringRefEquals(
      castToUTF16Ref(start, length), other->castToUTF16Ref());
}

bool StringPrimitive::equals(const StringPrimitive *other) const {
  if (this == other) {
    return true;
  }
  return sliceEquals(0, getStringLength(), other);
}

bool StringPrimitive::equals(const StringView &other) const {
  if (isASCII()) {
    return other.equals(castToASCIIRef());
  }
  return other.equals(castToUTF16Ref());
}

int StringPrimitive::compare(const StringPrimitive *other) const {
  if (isASCII()) {
    if (other->isASCII()) {
      return stringRefCompare(castToASCIIRef(), other->castToASCIIRef());
    }
    return stringRefCompare(castToASCIIRef(), other->castToUTF16Ref());
  }
  if (other->isASCII()) {
    return stringRefCompare(castToUTF16Ref(), other->castToASCIIRef());
  }
  return stringRefCompare(castToUTF16Ref(), other->castToUTF16Ref());
}

CallResult<HermesValue> StringPrimitive::concat(
    Runtime *runtime,
    Handle<StringPrimitive> xHandle,
    Handle<StringPrimitive> yHandle) {
  auto xLen = xHandle->getStringLength();
  auto yLen = yHandle->getStringLength();
  if (!xLen) {
    // x is the empty string, just return y.
    return yHandle.getHermesValue();
  }
  if (!yLen) {
    // y is the empty string, just return x.
    return xHandle.getHermesValue();
  }

  SafeUInt32 xyLen(xLen);
  xyLen.add(yLen);

  auto builder = StringBuilder::createStringBuilder(
      runtime, xyLen, xHandle->isASCII() && yHandle->isASCII());
  if (builder == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }

  builder->appendStringPrim(xHandle);
  builder->appendStringPrim(yHandle);
  return HermesValue::encodeStringValue(*builder->getStringPrimitive());
}

CallResult<HermesValue> StringPrimitive::slice(
    Runtime *runtime,
    Handle<StringPrimitive> str,
    size_t start,
    size_t length) {
  assert(
      start + length <= str->getStringLength() && "Invalid length for slice");

  SafeUInt32 safeLen(length);

  auto builder =
      StringBuilder::createStringBuilder(runtime, safeLen, str->isASCII());
  if (builder == ExecutionStatus::EXCEPTION) {
    return ExecutionStatus::EXCEPTION;
  }
  if (str->isASCII()) {
    builder->appendASCIIRef(
        ASCIIRef(str->castToASCIIPointer() + start, length));
  } else {
    builder->appendUTF16Ref(
        UTF16Ref(str->castToUTF16Pointer() + start, length));
  }
  return HermesValue::encodeStringValue(*builder->getStringPrimitive());
}

StringView StringPrimitive::createStringView(
    Runtime *runtime,
    Handle<StringPrimitive> self) {
  ensureFlat(runtime, self);
  return createStringViewMustBeFlat(self);
}

void StringPrimitive::copyUTF16String(
    llvm::SmallVectorImpl<char16_t> &str) const {
  if (isASCII()) {
    const char *ptr = castToASCIIPointer();
    str.append(ptr, ptr + getStringLength());
  } else {
    const char16_t *ptr = castToUTF16Pointer();
    str.append(ptr, ptr + getStringLength());
  }
}

void StringPrimitive::copyUTF16String(char16_t *ptr) const {
  if (isASCII()) {
    const char *src = castToASCIIPointer();
    std::copy(src, src + getStringLength(), ptr);
  } else {
    const char16_t *src = castToUTF16Pointer();
    std::copy(src, src + getStringLength(), ptr);
  }
}

StringView StringPrimitive::createStringViewMustBeFlat(
    Handle<StringPrimitive> self) {
  return StringView(self);
}

template <typename T, bool Uniqued>
DynamicStringPrimitive<T, Uniqued>::DynamicStringPrimitive(
    Runtime *runtime,
    Ref src)
    : DynamicStringPrimitive(runtime, (uint32_t)src.size()) {
  hermes::uninitializedCopy(
      src.begin(), src.end(), this->template getTrailingObjects<T>());
}

template <typename T, bool Uniqued>
CallResult<HermesValue> DynamicStringPrimitive<T, Uniqued>::create(
    Runtime *runtime,
    Ref str) {
  assert(!isExternalLength(str.size()) && "length should not be external");
  void *mem =
      runtime->alloc</*fixedSize*/ false>(allocationSize((uint32_t)str.size()));
  return HermesValue::encodeStringValue(
      (new (mem) DynamicStringPrimitive<T, Uniqued>(runtime, str)));
}

template <typename T, bool Uniqued>
CallResult<HermesValue> DynamicStringPrimitive<T, Uniqued>::createLongLived(
    Runtime *runtime,
    Ref str) {
  assert(!isExternalLength(str.size()) && "length should not be external");
  void *mem = runtime->allocLongLived(allocationSize((uint32_t)str.size()));
  return HermesValue::encodeStringValue(
      (new (mem) DynamicStringPrimitive<T, Uniqued>(runtime, str)));
}

template <typename T, bool Uniqued>
CallResult<HermesValue> DynamicStringPrimitive<T, Uniqued>::create(
    Runtime *runtime,
    uint32_t length) {
  void *mem = runtime->alloc</*fixedSize*/ false>(allocationSize(length));
  return HermesValue::encodeStringValue(
      (new (mem) DynamicStringPrimitive<T, Uniqued>(runtime, length)));
}

template class DynamicStringPrimitive<char16_t, true /* Uniqued */>;
template class DynamicStringPrimitive<char, true /* Uniqued */>;
template class DynamicStringPrimitive<char16_t, false /* not Uniqued */>;
template class DynamicStringPrimitive<char, false /* not Uniqued */>;

template <typename T>
ExternalStringPrimitive<T>::ExternalStringPrimitive(
    Runtime *runtime,
    StdString &&contents)
    : ExternalStringPrimitive(
          runtime,
          std::move(contents),
          false /* not uniqued */) {}

template <typename T>
ExternalStringPrimitive<T>::ExternalStringPrimitive(
    Runtime *runtime,
    StdString &&contents,
    SymbolID uniqueID)
    : ExternalStringPrimitive(
          runtime,
          std::move(contents),
          true /* uniqued */) {
  updateUniqueID(uniqueID);
}

template <typename T>
CallResult<HermesValue> ExternalStringPrimitive<T>::create(
    Runtime *runtime,
    StdString &&str) {
  if (LLVM_UNLIKELY(str.size() > MAX_STRING_LENGTH))
    return runtime->raiseRangeError("String length exceeds limit");
  uint32_t allocSize = str.size() * sizeof(T);
  void *mem = runtime->alloc</*fixedSize*/ true, HasFinalizer::Yes>(
      sizeof(ExternalStringPrimitive<T>));
  auto res = HermesValue::encodeStringValue(
      (new (mem) ExternalStringPrimitive<T>(runtime, std::move(str))));
  runtime->getHeap().creditExternalMemory(res.getString(), allocSize);
  return res;
}

template <typename T>
CallResult<HermesValue> ExternalStringPrimitive<T>::createLongLived(
    Runtime *runtime,
    StdString &&str,
    SymbolID uniqueID) {
  if (LLVM_UNLIKELY(str.size() > MAX_STRING_LENGTH))
    return runtime->raiseRangeError("String length exceeds limit");
  uint32_t allocSize = str.size() * sizeof(T);
  if (LLVM_UNLIKELY(!runtime->getHeap().canAllocExternalMemory(allocSize))) {
    return runtime->raiseRangeError(
        "Cannot allocate an external string primitive.");
  }
  void *mem = runtime->allocLongLived<HasFinalizer::Yes>(
      sizeof(ExternalStringPrimitive<T>));
  auto res = HermesValue::encodeStringValue((
      new (mem) ExternalStringPrimitive<T>(runtime, std::move(str), uniqueID)));
  runtime->getHeap().creditExternalMemory(res.getString(), allocSize);
  return res;
}

template <typename T>
CallResult<HermesValue> ExternalStringPrimitive<T>::create(
    Runtime *runtime,
    uint32_t length) {
  assert(isExternalLength(length) && "length should be external");
  if (LLVM_UNLIKELY(length > MAX_STRING_LENGTH))
    return runtime->raiseRangeError("String length exceeds limit");
  uint32_t allocSize = length * sizeof(T);
  if (LLVM_UNLIKELY(!runtime->getHeap().canAllocExternalMemory(allocSize))) {
    return runtime->raiseRangeError(
        "Cannot allocate an external string primitive.");
  }
  return create(runtime, StdString(length, T(0)));
}

template <typename T>
void ExternalStringPrimitive<T>::_finalizeImpl(GCCell *cell, GC *gc) {
  ExternalStringPrimitive<T> *self = vmcast<ExternalStringPrimitive<T>>(cell);
  gc->debitExternalMemory(self, self->getStringByteSize());
  self->~ExternalStringPrimitive<T>();
}

template <typename T>
size_t ExternalStringPrimitive<T>::_mallocSizeImpl(GCCell *cell) {
  ExternalStringPrimitive<T> *self = vmcast<ExternalStringPrimitive<T>>(cell);
  return self->getStringByteSize();
}

template class ExternalStringPrimitive<char16_t>;
template class ExternalStringPrimitive<char>;

} // namespace vm
} // namespace hermes
