// SPDX-License-Identifier: MIT
// Copyright (c) 2026, Leo Zhang.
//
// AirSignature.h (UniXNU-piano, tooling/metal/turnip-a830/frontend, ours): the signature of a visible (stitchable) function as its
// compiled AIR states it. The Metal front end writes it into the function's bitcode as module metadata: the named node !air.visible
// lists, per visible function, !{ptr @function, !outputs, !inputs, !{!"air.stitching_info", RESULT, ARGUMENT...}}, where RESULT is an
// air.stitching_type over a type node (null for void) and each ARGUMENT is !{!"air.stitching_argument", !stitching_type, !"name"}.
// Type nodes: !{!"air.<kind>_type", i32 size, i32 align, QUALIFIERS-or-null, ...}; structs name their fields (air.record_field:
// offset, offset alignment, type, name).
//
// Read here with a minimal LLVM bitstream reader (abbreviations, BLOCKINFO, the module's value list: globals, functions and module
// constants, the module-level metadata and the string table; function bodies are skipped). Every read is bounds-checked; anything
// this reader does not know (a node kind, a qualifier, a malformed record) makes read() fail with the reason: a signature is either
// the one the compiler wrote or none, never guessed. No Apple code or data is in this file.
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace air_signature {

struct Type;
struct Field {uint64_t offset = 0; std::string name; std::shared_ptr<Type> type;};
struct Type {
  enum Kind {Void, Scalar, Vector, Matrix, Struct, Array, Texture, Sampler, Pointer, Reference} kind = Void;
  uint64_t size = 0, align = 0;
  std::string scalar;              // Scalar: "float", "half", "int", ... (the air.<scalar>_type name)
  uint64_t count = 0;              // Vector: lanes; Array: length
  uint64_t columns = 0, rows = 0;  // Matrix
  std::string name;                // Struct
  std::vector<Field> fields;       // Struct
  std::shared_ptr<Type> element;   // Vector, Matrix, Array: element; Texture: texel type; Pointer, Reference: pointee
  int64_t addressSpace = -1;       // of this type when a pointer or reference points at it (air.address_space)
  std::string texture;             // Texture: "texture_2d", "depth_2d", ... (the air.<name>_type name)
  std::string access;              // Texture: "sample", "read", "write", "read_write"
};
struct Argument {std::string name; std::shared_ptr<Type> type;};
struct Signature {std::shared_ptr<Type> result; std::vector<Argument> arguments;};

namespace detail {
class Bits {
 public:
  Bits(const uint8_t *data, size_t length) : p_(data), bits_(length * 8) {}
  uint64_t fixed(unsigned width) {
    if (!width) return 0;
    if (width > 64 || pos_ + width > bits_) {bad_ = true; pos_ = bits_; return 0;}
    uint64_t v = 0; unsigned got = 0;
    while (got < width) {
      const size_t byte = pos_ >> 3; const unsigned off = pos_ & 7, take = std::min(8u - off, width - got);
      v |= (uint64_t)((p_[byte] >> off) & ((1u << take) - 1)) << got;
      got += take; pos_ += take;
    }
    return v;
  }
  uint64_t vbr(unsigned width) {
    if (width < 2 || width > 32) {bad_ = true; return 0;}
    uint64_t v = 0; unsigned shift = 0; const uint64_t high = 1ull << (width - 1);
    for (;;) {
      const uint64_t piece = fixed(width);
      if (bad_) return 0;
      if (shift < 64) v |= (piece & (high - 1)) << shift;
      if (!(piece & high)) return v;
      shift += width - 1;
      if (shift > 70) {bad_ = true; return 0;}
    }
  }
  void align32() {pos_ = (pos_ + 31) & ~size_t(31); if (pos_ > bits_) {bad_ = true; pos_ = bits_;}}
  bool bytes(size_t n, std::string &out) {
    if (pos_ & 7 || n > (bits_ - pos_) / 8) {bad_ = true; return false;}
    out.assign((const char *)p_ + pos_ / 8, n); pos_ += n * 8; return true;
  }
  size_t pos() const {return pos_;}
  size_t size() const {return bits_;}
  void seek(size_t at) {if (at > bits_) {bad_ = true; at = bits_;} pos_ = at;}
  bool bad() const {return bad_;}
  void fail() {bad_ = true;}
 private:
  const uint8_t *p_; size_t bits_, pos_ = 0; bool bad_ = false;
};
struct Op {enum Kind {Literal, Fixed, VBR, Array, Char6, Blob} kind; uint64_t value;};
using Abbrev = std::vector<Op>;
struct Value {enum Kind {Function, Global, Integer, Other} kind = Other; uint64_t offset = 0, size = 0; int64_t integer = 0;};
struct Metadata {enum Kind {String, Node, ValueRef, Other} kind = Other; std::string text; std::vector<uint64_t> operands; uint64_t value = 0;};
enum : unsigned {kBlockInfo = 0, kModule = 8, kConstants = 11, kMetadata = 15, kStrtab = 23};

class Reader {
 public:
  Reader(const uint8_t *data, size_t length) : b_(data, length) {}
  std::vector<Value> values;
  std::vector<Metadata> md;
  std::vector<std::pair<std::string, std::vector<uint64_t>>> named;
  std::string strtab;
  std::string why;

  bool parse() {
    if (b_.size() < 64 || b_.fixed(8) != 'B' || b_.fixed(8) != 'C' || b_.fixed(4) != 0x0 || b_.fixed(4) != 0xC ||
        b_.fixed(4) != 0xE || b_.fixed(4) != 0xD) return fail("not LLVM bitcode");
    while (!b_.bad() && b_.size() - b_.pos() >= 32) {
      const uint64_t id = b_.fixed(2);
      if (id == 0) break;   // END_BLOCK at the top level: the padding after the last block
      if (id != 1) return fail("a top-level record outside any block");
      if (!enter(~0u, 0)) return false;
    }
    if (b_.bad()) return fail("truncated bitcode");
    return true;
  }
  bool fail(const char *text) {if (why.empty()) why = text; return false;}

 private:
  Bits b_;
  std::map<unsigned, std::vector<Abbrev>> blockInfo_;
  int depth_ = 0;

  // ENTER_SUBBLOCK was read: the block's id, abbreviation width, length; a known block is read, any other skipped.
  bool enter(unsigned parent, unsigned) {
    const unsigned id = (unsigned)b_.vbr(8), width = (unsigned)b_.vbr(4);
    b_.align32();
    const uint64_t words = b_.fixed(32);
    if (b_.bad() || !width || width > 32 || words > (b_.size() - b_.pos()) / 32) return fail("a block past the end");
    const size_t end = b_.pos() + words * 32;
    const bool read = id == kBlockInfo || id == kModule || id == kStrtab ||
                      (parent == kModule && (id == kConstants || id == kMetadata));
    if (!read) {b_.seek(end); return !b_.bad();}
    if (++depth_ > 8) return fail("blocks nested too deep");
    const bool ok = block(id, width, end);
    depth_--;
    if (ok && b_.pos() != end) return fail("a block's end does not match its length");
    return ok;
  }
  bool defineAbbrev(Abbrev &a) {
    const uint64_t n = b_.vbr(5);
    if (n > 64) return fail("an abbreviation of too many operands");
    for (uint64_t i = 0; i < n && !b_.bad(); i++) {
      if (b_.fixed(1)) {a.push_back({Op::Literal, b_.vbr(8)}); continue;}
      const uint64_t e = b_.fixed(3);
      if (e == 1 || e == 2) {const uint64_t w = b_.vbr(5); if (w > 64 || (e == 2 && w < 2)) return fail("an abbreviation width"); a.push_back({e == 1 ? Op::Fixed : Op::VBR, w});}
      else if (e == 3) a.push_back({Op::Array, 0});
      else if (e == 4) a.push_back({Op::Char6, 0});
      else if (e == 5) a.push_back({Op::Blob, 0});
      else return fail("an unknown abbreviation encoding");
    }
    if (a.empty()) return fail("an empty abbreviation");
    return !b_.bad();
  }
  uint64_t scalar(const Op &op) {
    switch (op.kind) {
      case Op::Literal: return op.value;
      case Op::Fixed: return b_.fixed((unsigned)op.value);
      case Op::VBR: return b_.vbr((unsigned)op.value);
      case Op::Char6: {
        const uint64_t c = b_.fixed(6);
        return c < 26 ? 'a' + c : c < 52 ? 'A' + c - 26 : c < 62 ? '0' + c - 52 : c == 62 ? '.' : '_';
      }
      default: b_.fail(); return 0;
    }
  }
  bool record(unsigned abbrevId, const std::vector<Abbrev> &abbrevs, std::vector<uint64_t> &fields, std::string &blob) {
    fields.clear(); blob.clear();
    if (abbrevId == 3) {
      fields.push_back(b_.vbr(6));
      const uint64_t n = b_.vbr(6);
      if (n > (b_.size() - b_.pos())) return fail("a record past the end");
      for (uint64_t i = 0; i < n && !b_.bad(); i++) fields.push_back(b_.vbr(6));
      return !b_.bad();
    }
    if (abbrevId < 4 || abbrevId - 4 >= abbrevs.size()) return fail("an undefined abbreviation");
    const Abbrev &a = abbrevs[abbrevId - 4];
    for (size_t i = 0; i < a.size() && !b_.bad(); i++) {
      if (a[i].kind == Op::Array) {
        if (i + 1 >= a.size()) return fail("an array abbreviation without an element");
        const uint64_t n = b_.vbr(6);
        if (n > (b_.size() - b_.pos())) return fail("an array past the end");
        for (uint64_t k = 0; k < n && !b_.bad(); k++) fields.push_back(scalar(a[i + 1]));
        i++;
      } else if (a[i].kind == Op::Blob) {
        const uint64_t n = b_.vbr(6);
        b_.align32();
        if (!b_.bytes((size_t)n, blob)) return fail("a blob past the end");
        b_.align32();
      } else {
        fields.push_back(scalar(a[i]));
      }
    }
    if (fields.empty()) return fail("a record without a code");
    return !b_.bad();
  }
  bool block(unsigned id, unsigned width, size_t end) {
    std::vector<Abbrev> abbrevs = blockInfo_[id];
    std::vector<uint64_t> f; std::string blob;
    unsigned infoTarget = ~0u;
    std::string pendingName; bool havePendingName = false;
    while (!b_.bad()) {
      if (b_.pos() >= end) return fail("a block without its end");
      const unsigned a = (unsigned)b_.fixed(width);
      if (a == 0) {b_.align32(); return !b_.bad();}
      if (a == 1) {if (!enter(id, width)) return false; continue;}
      if (a == 2) {
        Abbrev abbrev; if (!defineAbbrev(abbrev)) return false;
        if (id == kBlockInfo) {if (infoTarget == ~0u) return fail("BLOCKINFO abbreviation before SETBID"); blockInfo_[infoTarget].push_back(abbrev);}
        else abbrevs.push_back(abbrev);
        continue;
      }
      if (!record(a, abbrevs, f, blob)) return false;
      const uint64_t code = f[0];
      const uint64_t *op = f.data() + 1; const size_t n = f.size() - 1;
      switch (id) {
        case kBlockInfo: if (code == 1 && n >= 1) infoTarget = (unsigned)op[0]; break;
        case kModule:
          if (code == 8 || code == 7 || code == 14 || code == 15 || code == 9) {   // FUNCTION, GLOBALVAR, ALIAS, IFUNC, ALIAS_OLD
            Value v; v.kind = code == 8 ? Value::Function : Value::Global;
            if (n >= 2) {v.offset = op[0]; v.size = op[1];}
            values.push_back(v);
          }
          break;
        case kConstants:
          if (code != 1) {   // every record but SETTYPE makes one value
            Value v;
            if (code == 4 && n >= 1) {v.kind = Value::Integer; v.integer = (op[0] & 1) ? -(int64_t)(op[0] >> 1) : (int64_t)(op[0] >> 1);}
            // CST_CODE_NULL: the writer's form of an integer zero (i32 0 in the type nodes) as of any null value; the metadata read
            // here only takes integers at these places, so it reads as 0
            else if (code == 2) {v.kind = Value::Integer; v.integer = 0;}
            values.push_back(v);
          }
          break;
        case kStrtab: if (code == 1) strtab = blob; break;
        case kMetadata: if (!metadata(code, op, n, blob, pendingName, havePendingName)) return false; break;
      }
    }
    return fail("truncated block");
  }
  bool metadata(uint64_t code, const uint64_t *op, size_t n, const std::string &blob, std::string &pendingName, bool &havePendingName) {
    if (md.size() > (1u << 22)) return fail("too much metadata");
    switch (code) {
      case 35: {   // METADATA_STRINGS: count, offset of the characters; the blob: count vbr6 lengths, then the characters
        if (n < 2 || op[1] > blob.size()) return fail("a malformed METADATA_STRINGS");
        Bits lengths((const uint8_t *)blob.data(), (size_t)op[1]);
        size_t at = (size_t)op[1];
        for (uint64_t i = 0; i < op[0]; i++) {
          const uint64_t len = lengths.vbr(6);
          if (lengths.bad() || len > blob.size() - at) return fail("a METADATA_STRINGS entry past its blob");
          Metadata m; m.kind = Metadata::String; m.text.assign(blob, at, (size_t)len); md.push_back(std::move(m)); at += (size_t)len;
        }
        return true;
      }
      case 1: {Metadata m; m.kind = Metadata::String; for (size_t i = 0; i < n; i++) m.text.push_back((char)op[i]); md.push_back(std::move(m)); return true;}
      case 2: {if (n < 2) return fail("a malformed METADATA_VALUE"); Metadata m; m.kind = Metadata::ValueRef; m.value = op[1]; md.push_back(std::move(m)); return true;}
      case 3: case 5: {Metadata m; m.kind = Metadata::Node; m.operands.assign(op, op + n); md.push_back(std::move(m)); return true;}
      case 4: pendingName.clear(); for (size_t i = 0; i < n; i++) pendingName.push_back((char)op[i]); havePendingName = true; return true;
      case 10:
        if (!havePendingName) return fail("METADATA_NAMED_NODE without a name");
        named.push_back({pendingName, std::vector<uint64_t>(op, op + n)}); havePendingName = false; return true;
      case 6: case 11: case 36: case 38: case 39: case 46: return true;   // KIND, ATTACHMENT, GLOBAL_DECL_ATTACHMENT, INDEX_OFFSET, INDEX, ARG_LIST
      default: md.push_back(Metadata()); return true;   // any other record (debug information) is one numbered node of no use here
    }
  }
};

class Builder {
 public:
  Builder(const Reader &r) : r_(r) {}
  std::string why;
  const Metadata *at(uint64_t id) const {return id < r_.md.size() ? &r_.md[id] : nullptr;}
  // A node operand: 0 is null, else the metadata id + 1.
  const Metadata *operand(const Metadata &node, size_t i) const {
    if (i >= node.operands.size() || !node.operands[i]) return nullptr;
    return at(node.operands[i] - 1);
  }
  bool isNull(const Metadata &node, size_t i) const {return i < node.operands.size() && !node.operands[i];}
  const std::string *string(const Metadata *m) const {return m && m->kind == Metadata::String ? &m->text : nullptr;}
  const std::string *head(const Metadata *m) const {return m && m->kind == Metadata::Node ? string(operand(*m, 0)) : nullptr;}
  bool integer(const Metadata *m, int64_t &out) const {
    if (!m || m->kind != Metadata::ValueRef || m->value >= r_.values.size() || r_.values[m->value].kind != Value::Integer) return false;
    out = r_.values[m->value].integer; return true;
  }
  std::string functionName(const Metadata *m) const {
    if (!m || m->kind != Metadata::ValueRef || m->value >= r_.values.size()) return std::string();
    const Value &v = r_.values[m->value];
    if (v.kind != Value::Function || v.offset > r_.strtab.size() || v.size > r_.strtab.size() - v.offset) return std::string();
    return r_.strtab.substr((size_t)v.offset, (size_t)v.size);
  }
  bool fail(std::string text) {if (why.empty()) why = std::move(text); return false;}

  bool type(const Metadata *m, std::shared_ptr<Type> &out, int depth = 0) {
    if (depth > 16) return fail("types nested too deep");
    const std::string *kind = head(m);
    if (!kind || kind->compare(0, 4, "air.") || kind->size() < 9 || kind->compare(kind->size() - 5, 5, "_type"))
      return fail("an argument type that is not an AIR type node");
    const std::string k = kind->substr(4, kind->size() - 9);
    auto t = std::make_shared<Type>();
    int64_t size = 0, align = 0;
    if (!integer(operand(*m, 1), size) || !integer(operand(*m, 2), align) || size < 0 || align < 0) return fail("a type without its size");
    t->size = (uint64_t)size; t->align = (uint64_t)align;
    if (!isNull(*m, 3)) {   // qualifiers: only the address space is known
      const Metadata *quals = operand(*m, 3);
      if (!quals || quals->kind != Metadata::Node) return fail("malformed type qualifiers");
      for (size_t i = 0; i < quals->operands.size(); i++) {
        const Metadata *q = operand(*quals, i);
        const std::string *name = head(q);
        int64_t space = 0;
        if (!name || *name != "air.address_space" || !integer(operand(*q, 1), space)) return fail("an unknown type qualifier");
        t->addressSpace = space;
      }
    }
    static const char *const scalars[] = {"float", "half", "int", "uint", "bool", "short", "ushort", "char", "uchar", "long", "ulong"};
    for (const char *s : scalars) if (k == s) {t->kind = Type::Scalar; t->scalar = s; out = t; return true;}
    int64_t a = 0, b = 0;
    if (k == "vector") {
      if (!type(operand(*m, 4), t->element, depth + 1) || t->element->kind != Type::Scalar || !integer(operand(*m, 5), a) || a < 2 || a > 4)
        return fail("a vector type of no scalar lanes");
      t->kind = Type::Vector; t->count = (uint64_t)a; out = t; return true;
    }
    if (k == "matrix") {
      if (!type(operand(*m, 4), t->element, depth + 1) || t->element->kind != Type::Scalar || !integer(operand(*m, 5), a) ||
          !integer(operand(*m, 6), b) || a < 2 || a > 4 || b < 2 || b > 4)
        return fail("a matrix type of no scalar elements");
      t->kind = Type::Matrix; t->columns = (uint64_t)a; t->rows = (uint64_t)b; out = t; return true;
    }
    if (k == "array") {
      if (!type(operand(*m, 4), t->element, depth + 1) || !integer(operand(*m, 5), a) || a < 0) return fail("a malformed array type");
      t->kind = Type::Array; t->count = (uint64_t)a; out = t; return true;
    }
    if (k == "struct") {
      const std::string *name = string(operand(*m, 4));
      const Metadata *fields = operand(*m, 5);
      if (!name || !fields || fields->kind != Metadata::Node) return fail("a struct type without its name or fields");
      t->kind = Type::Struct; t->name = *name;
      for (size_t i = 0; i < fields->operands.size(); i++) {
        const Metadata *f = operand(*fields, i);
        const std::string *fk = head(f);
        int64_t offset = 0;
        const std::string *fname = f ? string(operand(*f, 4)) : nullptr;
        if (!fk || *fk != "air.record_field" || !integer(operand(*f, 1), offset) || offset < 0 || !fname) return fail("a malformed struct field");
        Field field; field.offset = (uint64_t)offset; field.name = *fname;
        if (!type(operand(*f, 3), field.type, depth + 1)) return false;
        t->fields.push_back(std::move(field));
      }
      out = t; return true;
    }
    if (k == "sampler") {t->kind = Type::Sampler; out = t; return true;}
    if (k == "pointer" || k == "lvalue_reference") {
      if (!type(operand(*m, 4), t->element, depth + 1)) return false;
      t->kind = k == "pointer" ? Type::Pointer : Type::Reference; out = t; return true;
    }
    static const char *const textures[] = {"texture_1d", "texture_1d_array", "texture_2d", "texture_2d_array", "texture_2d_ms",
                                           "texture_2d_ms_array", "texture_3d", "texture_cube", "texture_cube_array", "texture_buffer",
                                           "depth_2d", "depth_2d_array", "depth_2d_ms", "depth_2d_ms_array", "depth_cube", "depth_cube_array"};
    for (const char *s : textures) if (k == s) {
      const std::string *access = string(operand(*m, 5));
      if (!type(operand(*m, 4), t->element, depth + 1) || t->element->kind != Type::Scalar || !access) return fail("a malformed texture type");
      t->kind = Type::Texture; t->texture = s; t->access = *access; out = t; return true;
    }
    return fail("an AIR type this reader does not know: air." + k + "_type");
  }
  bool stitchingType(const Metadata *m, std::shared_ptr<Type> &out) {
    const std::string *kind = head(m);
    if (!kind || *kind != "air.stitching_type") return fail("no air.stitching_type");
    return type(operand(*m, 1), out);
  }
 private:
  const Reader &r_;
};
}  // namespace detail

// The signature of `function` from its bitcode (a 0x0B17C0DE wrapper is unwrapped): false, with the reason, when the module has no
// stitching information for it or anything on the way is unknown.
inline bool read(const uint8_t *bitcode, size_t length, const std::string &function, Signature &out, std::string &why) {
  if (length >= 20 && bitcode[0] == 0xDE && bitcode[1] == 0xC0 && bitcode[2] == 0x17 && bitcode[3] == 0x0B) {
    uint32_t offset, size; memcpy(&offset, bitcode + 8, 4); memcpy(&size, bitcode + 12, 4);
    if (offset > length || size > length - offset) {why = "a bitcode wrapper past the end"; return false;}
    bitcode += offset; length = size;
  }
  detail::Reader reader(bitcode, length);
  if (!reader.parse()) {why = reader.why; return false;}
  detail::Builder b(reader);
  for (auto &named : reader.named) {
    if (named.first != "air.visible") continue;
    for (uint64_t id : named.second) {
      const detail::Metadata *entry = b.at(id);
      if (!entry || entry->kind != detail::Metadata::Node || b.functionName(b.operand(*entry, 0)) != function) continue;
      for (size_t i = 1; i < entry->operands.size(); i++) {
        const detail::Metadata *info = b.operand(*entry, i);
        const std::string *kind = b.head(info);
        if (!kind || *kind != "air.stitching_info") continue;
        Signature s;
        if (b.isNull(*info, 1)) {s.result = std::make_shared<Type>(); s.result->kind = Type::Void;}
        else if (!b.stitchingType(b.operand(*info, 1), s.result)) {why = b.why; return false;}
        for (size_t a = 2; a < info->operands.size(); a++) {
          const detail::Metadata *arg = b.operand(*info, a);
          const std::string *ak = b.head(arg);
          const std::string *name = arg ? b.string(b.operand(*arg, 2)) : nullptr;
          if (!ak || *ak != "air.stitching_argument" || !name) {why = "a malformed air.stitching_argument"; return false;}
          Argument argument; argument.name = *name;
          if (!b.stitchingType(b.operand(*arg, 1), argument.type)) {why = b.why; return false;}
          s.arguments.push_back(std::move(argument));
        }
        out = std::move(s);
        return true;
      }
      why = "the function is visible but has no air.stitching_info";
      return false;
    }
  }
  why = "no !air.visible entry for the function";
  return false;
}

}  // namespace air_signature
