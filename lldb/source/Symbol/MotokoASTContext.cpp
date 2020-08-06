//===-- MotokoASTContext.cpp ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <mutex>
#include <utility>
#include <vector>

#include "lldb/Core/Module.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/UniqueCStringMap.h"
#include "lldb/Core/ValueObject.h"
#include "lldb/DataFormatters/StringPrinter.h"
#include "lldb/Symbol/CompilerType.h"
#include "lldb/Symbol/MotokoASTContext.h"
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Target.h"
#include "lldb/Core/DumpDataExtractor.h"

#include "llvm/Support/Threading.h"

//#include "Plugins/ExpressionParser/Motoko/MotokoUserExpression.h"
#include "Plugins/ExpressionParser/Rust/RustUserExpression.h"
#include "Plugins/SymbolFile/DWARF/DWARFASTParserMotoko.h"

#include <unordered_map>

using namespace lldb;

namespace lldb_private {

class MotokoAggregateBase;
class MotokoArray;
class MotokoBool;
class MotokoCLikeEnum;
class MotokoEnum;
class MotokoFunction;
class MotokoIntegral;
class MotokoPointer;
class MotokoStruct;
class MotokoTuple;
class MotokoTypedef;

class MotokoType {
protected:

  MotokoType(const ConstString &name) : m_name(name) {}
  DISALLOW_COPY_AND_ASSIGN (MotokoType);

public:

  virtual ~MotokoType() {}

  ConstString Name() const { return m_name; }

  virtual lldb::Format Format() const {
    return eFormatBytes;
  }

  virtual std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                             const std::string &varname) = 0;

  virtual uint32_t TypeInfo(CompilerType *element_type) const = 0;
  virtual lldb::TypeClass TypeClass() const = 0;
  virtual uint64_t ByteSize() const = 0;

  virtual MotokoAggregateBase *AsAggregate() { return nullptr; }
  virtual MotokoArray *AsArray() { return nullptr; }
  virtual MotokoBool *AsBool() { return nullptr; }
  virtual MotokoCLikeEnum *AsCLikeEnum() { return nullptr; }
  virtual MotokoEnum *AsEnum() { return nullptr; }
  virtual MotokoFunction *AsFunction() { return nullptr; }
  virtual MotokoIntegral *AsInteger () { return nullptr; }
  virtual MotokoPointer *AsPointer () { return nullptr; }
  virtual MotokoTuple *AsTuple() { return nullptr; }
  virtual MotokoTypedef *AsTypedef() { return nullptr; }

  virtual bool IsAggregateType() const { return false; }
  virtual bool IsCharType() const { return false; }
  virtual bool IsFloatType() const { return false; }

private:
  ConstString m_name;
};

class MotokoBool : public MotokoType {
public:
  MotokoBool(const ConstString &name) : MotokoType(name) {}
  DISALLOW_COPY_AND_ASSIGN(MotokoBool);

  MotokoBool *AsBool() override {
    return this;
  }

  lldb::Format Format() const override {
    return eFormatBoolean;
  }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeIsBuiltIn | eTypeHasValue | eTypeIsScalar;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassBuiltin;
  }

  uint64_t ByteSize() const override {
    return 1;
  }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    return "bool " + varname;
  }
};

class MotokoIntegral : public MotokoType {
public:
  MotokoIntegral(const ConstString &name, bool is_signed, uint64_t byte_size,
               bool is_char = false)
    : MotokoType(name),
      m_is_signed(is_signed),
      m_byte_size(byte_size),
      m_is_char(is_char)
  {}
  DISALLOW_COPY_AND_ASSIGN(MotokoIntegral);

  lldb::Format Format() const override {
    if (m_is_char)
      return eFormatUnicode32;
    return m_is_signed ? eFormatDecimal : eFormatUnsigned;
  }

  bool IsSigned() const { return m_is_signed; }
  uint64_t ByteSize() const override { return m_byte_size; }

  MotokoIntegral *AsInteger () override { return this; }

  bool IsCharType() const override { return m_is_char; }

  uint32_t TypeInfo(CompilerType *) const override {
    uint32_t result = eTypeIsBuiltIn | eTypeHasValue | eTypeIsScalar | eTypeIsInteger;
    if (m_is_signed)
      result |= eTypeIsSigned;
    return result;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassBuiltin;
  }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    // These names are predefined by clang.
    std::string result = "__";
    if (!m_is_signed) {
      result += "U";
    }
    result += "INT" + std::to_string(8 * m_byte_size) + "_TYPE__ " + varname;
    return result;
  }

private:

  bool m_is_signed;
  uint64_t m_byte_size;
  bool m_is_char;
};

class MotokoCLikeEnum : public MotokoType {
public:
  MotokoCLikeEnum(const ConstString &name, const CompilerType &underlying_type,
                std::map<uint32_t, std::string> &&values)
    : MotokoType(name),
      m_underlying_type(underlying_type),
      m_values(std::move(values))
  {
  }
  DISALLOW_COPY_AND_ASSIGN(MotokoCLikeEnum);

  MotokoCLikeEnum *AsCLikeEnum() override { return this; }

  lldb::Format Format() const override {
    return eFormatEnum;
  }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeHasValue | eTypeIsEnumeration | eTypeIsScalar;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassEnumeration;
  }

  uint64_t ByteSize() const override {
    return 4;
  }

  bool IsSigned() const {
    bool is_signed;
    return m_underlying_type.IsIntegerType(is_signed) && is_signed;
  }

  bool FindName(uint64_t val, std::string &name) {
    auto iter = m_values.find(val);
    if (iter == m_values.end()) {
      return false;
    }
    name = iter->second;
    return true;
  }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    MotokoType *type = (MotokoType *) m_underlying_type.GetOpaqueQualType();
    return type->GetCABITypeDeclaration(name_map, varname);
  }

private:

  CompilerType m_underlying_type;
  std::map<uint32_t, std::string> m_values;
};

class MotokoFloat : public MotokoType {
public:
  MotokoFloat(const ConstString &name, uint64_t byte_size)
    : MotokoType(name),
      m_byte_size(byte_size)
  {}
  DISALLOW_COPY_AND_ASSIGN(MotokoFloat);

  lldb::Format Format() const override {
    return eFormatFloat;
  }

  bool IsFloatType() const override { return true; }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeIsBuiltIn | eTypeHasValue | eTypeIsFloat;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassBuiltin;
  }

  uint64_t ByteSize() const override { return m_byte_size; }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    return (m_byte_size == 4 ? "float " : "double ") + varname;
  }

private:

  uint64_t m_byte_size;
};

class MotokoPointer : public MotokoType {
public:
  // Pointers and references are handled similarly.
  MotokoPointer(const ConstString &name, const CompilerType &pointee, uint64_t byte_size)
    : MotokoType(name),
      m_pointee(pointee),
      m_byte_size(byte_size)
  {}
  DISALLOW_COPY_AND_ASSIGN(MotokoPointer);

  lldb::Format Format() const override {
    return eFormatPointer;
  }

  CompilerType PointeeType() const { return m_pointee; }

  MotokoPointer *AsPointer() override { return this; }

  uint32_t TypeInfo(CompilerType *elem) const override {
    if (elem)
      *elem = m_pointee;
    return eTypeIsBuiltIn | eTypeHasValue | eTypeIsPointer;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassPointer;
  }

  uint64_t ByteSize() const override {
    return m_byte_size;
  }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    MotokoType *p_type = (MotokoType *) m_pointee.GetOpaqueQualType();
    if (p_type->AsFunction()) {
      // This does the right thing, see the implementation.
      return p_type->GetCABITypeDeclaration(name_map, varname);
    }
    return p_type->GetCABITypeDeclaration(name_map, "") + "* " + varname;
  }

private:

  CompilerType m_pointee;
  uint64_t m_byte_size;
};

class MotokoArray : public MotokoType {
public:
  MotokoArray(const ConstString &name, uint64_t length, const CompilerType &elem)
    : MotokoType(name),
      m_length(length),
      m_elem(elem)
  {}
  DISALLOW_COPY_AND_ASSIGN(MotokoArray);

  uint64_t Length() const { return m_length; }
  MotokoArray *AsArray() override { return this; }
  CompilerType ElementType() const { return m_elem; }
  bool IsAggregateType() const override { return true; }

  uint32_t TypeInfo(CompilerType *elem) const override {
    if (elem)
      *elem = m_elem;
    return eTypeHasChildren | eTypeIsArray;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassArray;
  }

  uint64_t ByteSize() const override {
    return *m_elem.GetByteSize(nullptr) * m_length;
  }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    MotokoType *type = static_cast<MotokoType *>(m_elem.GetOpaqueQualType());
    return type->GetCABITypeDeclaration(name_map, varname)
      + "[" + std::to_string(m_length) + "]";
  }

private:
  uint64_t m_length;
  CompilerType m_elem;
};

// Base type for struct, tuple, and tuple struct.
class MotokoAggregateBase : public MotokoType {
protected:
  MotokoAggregateBase(const ConstString &name, uint64_t byte_size, bool has_discriminant = false)
    : MotokoType(name),
      m_byte_size(byte_size),
      m_has_discriminant(has_discriminant)
  {}

  DISALLOW_COPY_AND_ASSIGN(MotokoAggregateBase);

public:

  MotokoAggregateBase *AsAggregate() override { return this; }

  bool IsAggregateType() const override { return true; }

  size_t FieldCount() const { return m_fields.size(); }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeHasChildren | eTypeIsStructUnion;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassStruct;
  }

  uint64_t ByteSize() const override {
    return m_byte_size;
  }

  struct Field {
    Field(const ConstString &name, const CompilerType &type, uint64_t offset)
      : m_name(name),
        m_type(type),
        m_offset(offset)
    {
    }

    ConstString m_name;
    CompilerType m_type;
    uint64_t m_offset;
  };

  void AddField(const ConstString &name, const CompilerType &type, uint64_t offset) {
    m_fields.emplace_back(name, type, offset);
  }

  void AddTemplateParameter(const CompilerType &ctype) {
    m_template_args.push_back(ctype);
  }

  virtual void FinishInitialization() {
  }

  bool HasDiscriminant() const {
    return m_has_discriminant;
  }

  // With the old-style enum encoding, after the discriminant's
  // location is computed the member types no longer need to have
  // theirs, so they are dropped.
  virtual void DropDiscriminant() {
    if (m_has_discriminant) {
      m_has_discriminant = false;
      m_fields.erase(m_fields.begin());
    }
  }

  const Field *FieldAt(size_t idx) {
    if (idx >= m_fields.size())
      return nullptr;
    return &m_fields[idx];
  }

  size_t GetNumTemplateArguments() const {
    return m_template_args.size();
  }

  CompilerType GetTypeTemplateArgument(size_t idx) const {
    return m_template_args[idx];
  }

  typedef std::vector<Field>::const_iterator const_iterator;

  const_iterator begin() const {
    return m_fields.begin();
  }

  const_iterator end() const {
    return m_fields.end();
  }

  // Type-printing support.
  virtual const char *Tag() const = 0;

  virtual const char *TagName() const {
    return Name().AsCString();
  }

  virtual const char *Opener() const = 0;
  virtual const char *Closer() const = 0;

protected:

  std::string GetFieldsCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map) {
    int argno = 0;
    std::string result;
    for (const Field &f : m_fields) {
      MotokoType *rtype = static_cast<MotokoType *>(f.m_type.GetOpaqueQualType());
      std::string name;
      if (f.m_name.IsEmpty()) {
        name = "__" + std::to_string(argno++);
      } else {
        name = std::string("_") + f.m_name.AsCString();
      }
      result += rtype->GetCABITypeDeclaration(name_map, name) + "; ";
    }
    return result;
  }

  Field *MutableFieldAt(size_t idx) {
    if (idx >= m_fields.size())
      return nullptr;
    return &m_fields[idx];
  }

private:

  uint64_t m_byte_size;
  std::vector<Field> m_fields;
  bool m_has_discriminant;
  std::vector<CompilerType> m_template_args;
};

class MotokoTuple : public MotokoAggregateBase {
public:
  MotokoTuple(const ConstString &name, uint64_t byte_size, bool has_discriminant)
    : MotokoAggregateBase(name, byte_size, has_discriminant)
  {}

  DISALLOW_COPY_AND_ASSIGN(MotokoTuple);

  MotokoTuple *AsTuple() override { return this; }

  void AddField(const CompilerType &type, uint64_t offset) {
    MotokoAggregateBase::AddField(ConstString(), type, offset);
  }

  const char *Tag() const override {
    return IsTuple() ? "" : "struct ";
  }
  const char *TagName() const override {
    if (IsTuple()) {
      return "";
    }
    return Name().AsCString();
  }
  const char *Opener() const override {
    return "(";
  }
  const char *Closer() const override {
    return ")";
  }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    std::string tagname;
    if (name_map->Tag(this, &tagname)) {
      std::string def = "  struct " + tagname + "{"
        + GetFieldsCABITypeDeclaration(name_map) + " };\n";
      name_map->typedefs.append(def);
    }
    return tagname + " " + varname;
  }

  void DropDiscriminant() override {
    MotokoAggregateBase::DropDiscriminant();
    // Rename the fields, because we dropped the first one.
    for (size_t i = 0; i < FieldCount(); ++i) {
      Field *f = MutableFieldAt(i);
      char buf[32];
      snprintf (buf, sizeof (buf), "%u", unsigned(i));
      f->m_name = ConstString(buf);
    }
  }

private:

  // As opposed to a tuple struct.
  bool IsTuple() const {
    ConstString name = Name();
    // For the time being we must examine the name, because the DWARF
    // doesn't provide anything else.
    return name.IsEmpty() || name.AsCString()[0] == '(';
  }
};

class MotokoStruct : public MotokoAggregateBase {
public:
  MotokoStruct(const ConstString &name, uint64_t byte_size, bool has_discriminant)
    : MotokoAggregateBase(name, byte_size, has_discriminant)
  {}

  DISALLOW_COPY_AND_ASSIGN(MotokoStruct);

  const char *Tag() const override {
    return "struct ";
  }
  const char *Opener() const override {
    return "{";
  }
  const char *Closer() const override {
    return "}";
  }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    std::string tagname;
    if (name_map->Tag(this, &tagname)) {
      std::string def = "  struct " + tagname + "{"
        + GetFieldsCABITypeDeclaration(name_map) + " };\n";
      name_map->typedefs.append(def);
    }
    return tagname + " " + varname;
  }
};

class MotokoUnion : public MotokoAggregateBase {
public:
  MotokoUnion(const ConstString &name, uint64_t byte_size)
    : MotokoAggregateBase(name, byte_size)
  {}

  DISALLOW_COPY_AND_ASSIGN(MotokoUnion);

  const char *Tag() const override {
    return "union ";
  }
  const char *Opener() const override {
    return "{";
  }
  const char *Closer() const override {
    return "}";
  }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    std::string tagname;
    if (name_map->Tag(this, &tagname)) {
      std::string def = "  union " + tagname + "{"
        + GetFieldsCABITypeDeclaration(name_map) + " };\n";
      name_map->typedefs.append(def);
    }
    return tagname + " " + varname;
  }
};

// A Motoko enum, not a C-like enum.
class MotokoEnum : public MotokoAggregateBase {
public:
  MotokoEnum(const ConstString &name, uint64_t byte_size,
           uint32_t discr_offset, uint32_t discr_byte_size)
    : MotokoAggregateBase(name, byte_size),
      m_discr_offset(discr_offset),
      m_discr_byte_size(discr_byte_size),
      m_default(-1)
  {}

  DISALLOW_COPY_AND_ASSIGN(MotokoEnum);

  MotokoEnum *AsEnum() override { return this; }

  const char *Tag() const override {
    return "enum ";
  }
  const char *Opener() const override {
    return "{";
  }
  const char *Closer() const override {
    return "}";
  }

  // Record the discriminant for the most recently added field.
  void RecordDiscriminant(bool is_default, uint64_t discriminant) {
    int value = int(FieldCount() - 1);
    if (is_default) {
      m_default = value;
    } else {
      m_discriminants[discriminant] = value;
    }
  }

  void GetDiscriminantLocation(uint64_t &discr_offset, uint64_t &discr_byte_size) {
    discr_offset = m_discr_offset;
    discr_byte_size = m_discr_byte_size;
  }

  CompilerType FindEnumVariant(uint64_t discriminant) {
    auto iter = m_discriminants.find(discriminant);
    int idx = m_default;
    if (iter != m_discriminants.end()) {
      idx = iter->second;
    } else if (idx == -1) {
      // If the DWARF was bad somehow, we could end up not finding the
      // discriminant and not having a default.
      return CompilerType();
    }
    return FieldAt(idx)->m_type;
  }

  void FinishInitialization() override {
    for (auto&& iter : *this) {
      MotokoType *rtype = static_cast<MotokoType *>(iter.m_type.GetOpaqueQualType());
      if (MotokoAggregateBase* agg = rtype->AsAggregate()) {
        agg->DropDiscriminant();
      }
    }
  }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    std::string tagname;
    if (name_map->Tag(this, &tagname)) {
      std::string def = "struct " + tagname + "{ ";
      // If the discriminant comes first, then it is a hidden field,
      // which we'll emit.  Otherwise, it is in a hole somewhere, or
      // perhaps overlaid with some other field, so we don't bother.
      // (This is unwarranted compiler knowledge - FIXME.)  If there are
      // zero or one fields then there is no discriminant.
      if (FieldCount() > 1 && m_discr_offset == 0) {
        def += "int" + std::to_string(8 * m_discr_byte_size) + "_t __discr; ";
      }
      def += GetFieldsCABITypeDeclaration(name_map) + " };\n";
      name_map->typedefs.append(def);
    }
    return tagname + " " + varname;
  }

private:

  // The offset and byte size of the discriminant.  Note that, as a
  // special case, if there is only a single field then the
  // discriminant will be assumed not to exist.
  uint32_t m_discr_offset;
  uint32_t m_discr_byte_size;

  // The index in m_fields of the default variant.  -1 if there is no
  // default variant.
  int m_default;

  // This maps from discriminant values to indices in m_fields.  This
  // is used to find the correct variant given a discriminant value.
  std::unordered_map<uint64_t, int> m_discriminants;
};

class MotokoFunction : public MotokoType {
public:
  MotokoFunction (const ConstString &name, uint64_t byte_size,
                const CompilerType &return_type,
                const std::vector<CompilerType> &&arguments,
                const std::vector<CompilerType> &&template_arguments)
    : MotokoType(name),
      m_byte_size(byte_size),
      m_return_type(return_type),
      m_arguments(std::move(arguments)),
      m_template_args(std::move(template_arguments))
  {
  }
  DISALLOW_COPY_AND_ASSIGN(MotokoFunction);

  // do we care about the names?
  void AddArgument(const CompilerType &type) {
    m_arguments.push_back(type);
  }

  MotokoFunction *AsFunction() override { return this; }

  CompilerType ReturnType() const { return m_return_type; }
  size_t ArgumentCount() { return m_arguments.size(); }
  CompilerType Argument(size_t i) { return m_arguments[i]; }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeIsFuncPrototype | eTypeHasValue;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassFunction;
  }

  uint64_t ByteSize() const override {
    return m_byte_size;
  }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    MotokoType *type = (MotokoType *) m_return_type.GetOpaqueQualType();

    std::string result = type->GetCABITypeDeclaration(name_map, "") + " (*" +
      varname + ")(";
    bool first = true;
    for (CompilerType &iter : m_arguments) {
      MotokoType *type = (MotokoType *) iter.GetOpaqueQualType();
      if (!first) {
        result += ", ";
      }
      first = false;
      result += type->GetCABITypeDeclaration(name_map, "");
    }

    return result + ")";
  }

  size_t GetNumTemplateArguments() const {
    return m_template_args.size();
  }

  CompilerType GetTypeTemplateArgument(size_t idx) const {
    return m_template_args[idx];
  }

private:

  uint64_t m_byte_size;
  CompilerType m_return_type;
  std::vector<CompilerType> m_arguments;
  std::vector<CompilerType> m_template_args;
};

class MotokoTypedef : public MotokoType {
public:

  MotokoTypedef(const ConstString &name, const CompilerType &type)
    : MotokoType(name),
      m_type(type)
  {
  }

  DISALLOW_COPY_AND_ASSIGN(MotokoTypedef);

  MotokoTypedef *AsTypedef() override { return this; }
  CompilerType UnderlyingType() const { return m_type; }

  uint32_t TypeInfo(CompilerType *) const override {
    return eTypeIsTypedef;
  }

  lldb::TypeClass TypeClass() const override {
    return eTypeClassTypedef;
  }

  uint64_t ByteSize() const override {
    return *m_type.GetByteSize(nullptr);
  }

  std::string GetCABITypeDeclaration(MotokoASTContext::TypeNameMap *name_map,
                                     const std::string &varname) override {
    MotokoType *type = (MotokoType *) m_type.GetOpaqueQualType();
    return type->GetCABITypeDeclaration(name_map, varname);
  }

private:
  CompilerType m_type;
};

class MotokoDecl;
class MotokoDeclContext;

class MotokoDeclBase {
public:

  ConstString Name() const {
    return m_name;
  }

  ConstString QualifiedName() {
    if (!m_parent) {
      return m_name;
    }
    if (!m_full_name) {
      ConstString basename = m_parent->QualifiedName();
      if (basename) {
        std::string qual = std::string(basename.AsCString()) + "::" + m_name.AsCString();
        m_full_name = ConstString(qual.c_str());
      } else {
        m_full_name = m_name;
      }
    }
    return m_full_name;
  }

  MotokoDeclContext *Context() const {
    // Always succeeds.
    return m_parent->AsDeclContext();
  }

  virtual MotokoDecl *AsDecl() { return nullptr; }
  virtual MotokoDeclContext *AsDeclContext() { return nullptr; }

  virtual ~MotokoDeclBase() { }

protected:

  MotokoDeclBase(const ConstString &name, MotokoDeclBase *parent)
    : m_name(name),
      m_parent(parent)
  {
  }

private:

  ConstString m_name;
  // This is really a MotokoDeclContext.
  MotokoDeclBase *m_parent;
  ConstString m_full_name;
};

class MotokoDeclContext : public MotokoDeclBase {
public:
  MotokoDeclContext(const ConstString &name, MotokoDeclContext *parent)
    : MotokoDeclBase(name, parent)
  {
  }

  MotokoDeclContext *AsDeclContext() override { return this; }

  MotokoDeclBase *FindByName(const ConstString &name) {
    auto iter = m_decls.find(name);
    if (iter == m_decls.end()) {
      return nullptr;
    }
    return iter->second.get();
  }

  void AddItem(std::unique_ptr<MotokoDeclBase> &&item) {
    ConstString name = item->Name();
    m_decls[name] = std::move(item);
  }

private:
  std::map<ConstString, std::unique_ptr<MotokoDeclBase>> m_decls;
};

class MotokoDecl : public MotokoDeclBase {
public:
  MotokoDecl(const ConstString &name, const ConstString &mangled, MotokoDeclContext *parent)
    : MotokoDeclBase(name, parent),
      m_mangled(mangled)
  {
    assert(parent);
  }

  MotokoDecl *AsDecl() override { return this; }

  ConstString MangledName() const {
    return m_mangled;
  }

private:

  ConstString m_mangled;
};

} // namespace lldb_private
using namespace lldb_private;

MotokoASTContext::MotokoASTContext()
    : m_pointer_byte_size(0)
{
}

MotokoASTContext::~MotokoASTContext() {}

char MotokoASTContext::ID;

//------------------------------------------------------------------
// PluginInterface functions
//------------------------------------------------------------------

ConstString MotokoASTContext::GetPluginNameStatic() {
  return ConstString("rust");
}

ConstString MotokoASTContext::GetPluginName() {
  return MotokoASTContext::GetPluginNameStatic();
}

uint32_t MotokoASTContext::GetPluginVersion() {
  return 1;
}

lldb::TypeSystemSP MotokoASTContext::CreateInstance(lldb::LanguageType language,
                                                  Module *module,
                                                  Target *target) {
  if (language == eLanguageTypeMotoko) {
    ArchSpec arch;
    std::shared_ptr<MotokoASTContext> astc;
    if (module) {
      arch = module->GetArchitecture();
      astc = std::shared_ptr<MotokoASTContext>(new MotokoASTContext);
    } else if (target) {
      arch = target->GetArchitecture();
      astc = std::shared_ptr<MotokoASTContextForExpr>(
          new MotokoASTContextForExpr(target->shared_from_this()));
    }

    if (arch.IsValid()) {
      astc->SetAddressByteSize(arch.GetAddressByteSize());
      return astc;
    }
  }
  return lldb::TypeSystemSP();
}

void MotokoASTContext::Initialize() {
  static LanguageSet s_supported_languages_for_types;
  s_supported_languages_for_types.Insert(lldb::eLanguageTypeMotoko);
  static LanguageSet s_supported_languages_for_expressions;
  PluginManager::RegisterPlugin(GetPluginNameStatic(), "Motoko AST context plug-in",
                                CreateInstance,
				s_supported_languages_for_types,
				s_supported_languages_for_expressions);
}

void MotokoASTContext::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

//----------------------------------------------------------------------
// Tests
//----------------------------------------------------------------------

bool MotokoASTContext::IsArrayType(lldb::opaque_compiler_type_t type,
                                 CompilerType *element_type, uint64_t *size,
                                 bool *is_incomplete) {
  if (element_type)
    element_type->Clear();
  if (size)
    *size = 0;
  if (is_incomplete)
    *is_incomplete = false;
  MotokoArray *array = static_cast<MotokoType *>(type)->AsArray();
  if (array) {
    if (size)
      *size = array->Length();
    if (element_type)
      *element_type = array->ElementType();
    return true;
  }
  return false;
}

bool MotokoASTContext::IsVectorType(lldb::opaque_compiler_type_t type,
                                  CompilerType *element_type, uint64_t *size) {
  if (element_type)
    element_type->Clear();
  if (size)
    *size = 0;
  return false;
}

bool MotokoASTContext::IsAggregateType(lldb::opaque_compiler_type_t type) {
  return static_cast<MotokoType *>(type)->IsAggregateType();
}

bool MotokoASTContext::IsBeingDefined(lldb::opaque_compiler_type_t type) {
  return false;
}

bool MotokoASTContext::IsCharType(lldb::opaque_compiler_type_t type) {
  return static_cast<MotokoType *>(type)->IsCharType();
}

bool MotokoASTContext::IsCompleteType(lldb::opaque_compiler_type_t type) {
  return bool(type);
}

bool MotokoASTContext::IsConst(lldb::opaque_compiler_type_t type) {
  return false;
}

bool MotokoASTContext::IsCStringType(lldb::opaque_compiler_type_t type,
                                   uint32_t &length) {
  return false;
}

bool MotokoASTContext::IsDefined(lldb::opaque_compiler_type_t type) {
  return type != nullptr;
}

bool MotokoASTContext::IsFloatingPointType(lldb::opaque_compiler_type_t type,
                                         uint32_t &count, bool &is_complex) {
  is_complex = false;
  if (static_cast<MotokoType *>(type)->IsFloatType()) {
    count = 1;
    return true;
  }
  count = 0;
  return false;
}

bool MotokoASTContext::IsFunctionType(lldb::opaque_compiler_type_t type,
                                    bool *is_variadic_ptr) {
  if (is_variadic_ptr)
    *is_variadic_ptr = false;
  return static_cast<MotokoType *>(type)->AsFunction() != nullptr;
}

uint32_t MotokoASTContext::IsHomogeneousAggregate(lldb::opaque_compiler_type_t type,
                                                CompilerType *base_type_ptr) {
  // FIXME should detect "homogeneous floating-point aggregates".
  return false;
}

bool MotokoASTContext::CanPassInRegisters(const CompilerType &type) {
  /*if (auto *record_decl =
      ClangASTContext::GetAsRecordDecl(type)) {
    return record_decl->canPassInRegisters();
    }*/
  return false;
}



size_t
MotokoASTContext::GetNumberOfFunctionArguments(lldb::opaque_compiler_type_t type) {
  MotokoFunction *func = static_cast<MotokoType *>(type)->AsFunction();
  if (func) {
    return func->ArgumentCount();
  }
  return -1;
}

CompilerType
MotokoASTContext::GetFunctionArgumentAtIndex(lldb::opaque_compiler_type_t type,
                                           const size_t index) {
  MotokoFunction *func = static_cast<MotokoType *>(type)->AsFunction();
  if (func) {
    return func->Argument(index);
  }
  return CompilerType();
}

bool MotokoASTContext::IsFunctionPointerType(lldb::opaque_compiler_type_t type) {
  CompilerType pointee;
  if (!IsPointerType(type, &pointee)) {
    return false;
  }
  return pointee.IsFunctionType();
}

bool MotokoASTContext::IsBlockPointerType(lldb::opaque_compiler_type_t type,
                                        CompilerType *function_pointer_type_ptr) {
  return false;
}

bool MotokoASTContext::IsIntegerType(lldb::opaque_compiler_type_t type,
                                   bool &is_signed) {
  if (!type)
    return false;

  MotokoIntegral *inttype = static_cast<MotokoType *>(type)->AsInteger();
  if (inttype) {
    is_signed = inttype->IsSigned();
    return true;
  }
  return false;
}

bool MotokoASTContext::IsPolymorphicClass(lldb::opaque_compiler_type_t type) {
  return false;
}

bool MotokoASTContext::IsPossibleDynamicType(lldb::opaque_compiler_type_t type,
                                           CompilerType *target_type, // Can pass NULL
                                           bool check_cplusplus, bool check_objc) {
  if (target_type)
    target_type->Clear();
  // FIXME eventually we'll handle trait object pointers here
  if (static_cast<MotokoType *>(type)->AsEnum()) {
    return true;
  }
  return false;
}

bool MotokoASTContext::IsRuntimeGeneratedType(lldb::opaque_compiler_type_t type) {
  return false;
}

bool MotokoASTContext::IsPointerType(lldb::opaque_compiler_type_t type,
                                   CompilerType *pointee_type) {
  if (!type)
    return false;
  if (MotokoPointer *ptr = static_cast<MotokoType *>(type)->AsPointer()) {
    if (pointee_type)
      *pointee_type = ptr->PointeeType();
    return true;
  }
  return false;
}

bool MotokoASTContext::IsPointerOrReferenceType(lldb::opaque_compiler_type_t type,
                                              CompilerType *pointee_type) {
  return IsPointerType(type, pointee_type);
}

bool MotokoASTContext::IsReferenceType(lldb::opaque_compiler_type_t type,
                                     CompilerType *pointee_type,
                                     bool *is_rvalue) {
  return false;
}

bool MotokoASTContext::IsScalarType(lldb::opaque_compiler_type_t type) {
  return !IsAggregateType(type);
}

bool MotokoASTContext::IsTypedefType(lldb::opaque_compiler_type_t type) {
  if (type)
    return static_cast<MotokoType *>(type)->AsTypedef() != nullptr;
  return false;
}

bool MotokoASTContext::IsBooleanType(lldb::opaque_compiler_type_t type) {
  if (type)
    return static_cast<MotokoType *>(type)->AsBool() != nullptr;
  return false;
}

bool MotokoASTContext::IsVoidType(lldb::opaque_compiler_type_t type) {
  if (!type)
    return false;
  MotokoTuple *tuple = static_cast<MotokoType *>(type)->AsTuple();
  return tuple && !tuple->Name().IsEmpty() &&
    strcmp(tuple->Name().AsCString(), "()") == 0 && tuple->FieldCount() == 0;
}

bool MotokoASTContext::SupportsLanguage(lldb::LanguageType language) {
  return language == eLanguageTypeMotoko;
}

//----------------------------------------------------------------------
// Type Completion
//----------------------------------------------------------------------

bool MotokoASTContext::GetCompleteType(lldb::opaque_compiler_type_t type) {
  return bool(type);
}

//----------------------------------------------------------------------
// AST related queries
//----------------------------------------------------------------------

uint32_t MotokoASTContext::GetPointerByteSize() {
  return m_pointer_byte_size;
}

//----------------------------------------------------------------------
// Accessors
//----------------------------------------------------------------------

ConstString MotokoASTContext::GetTypeName(lldb::opaque_compiler_type_t type) {
  if (type)
    return static_cast<MotokoType *>(type)->Name();
  return ConstString();
}

uint32_t
MotokoASTContext::GetTypeInfo(lldb::opaque_compiler_type_t type,
                            CompilerType *pointee_or_element_compiler_type) {
  if (pointee_or_element_compiler_type)
    pointee_or_element_compiler_type->Clear();
  if (!type)
    return 0;
  return static_cast<MotokoType *>(type)->TypeInfo(pointee_or_element_compiler_type);
}

lldb::TypeClass MotokoASTContext::GetTypeClass(lldb::opaque_compiler_type_t type) {
  if (!type)
    return eTypeClassInvalid;
  return static_cast<MotokoType *>(type)->TypeClass();
}

lldb::BasicType
MotokoASTContext::GetBasicTypeEnumeration(lldb::opaque_compiler_type_t type) {
  ConstString name = GetTypeName(type);
  if (name.IsEmpty()) {
    // Nothing.
  } else if (strcmp(name.AsCString(), "()") == 0) {
    return eBasicTypeVoid;
  } else if (strcmp(name.AsCString(), "bool") == 0) {
    return eBasicTypeBool;
  }
  return eBasicTypeInvalid;
}

lldb::LanguageType
MotokoASTContext::GetMinimumLanguage(lldb::opaque_compiler_type_t type) {
  return lldb::eLanguageTypeMotoko;
}

unsigned MotokoASTContext::GetTypeQualifiers(lldb::opaque_compiler_type_t type) {
  return 0;
}

//----------------------------------------------------------------------
// Creating related types
//----------------------------------------------------------------------

CompilerType
MotokoASTContext::GetArrayElementType(lldb::opaque_compiler_type_t type,
                                    uint64_t *stride) {
  MotokoArray *array = static_cast<MotokoType *>(type)->AsArray();
  if (array) {
    if (stride) {
      *stride = *array->ElementType().GetByteSize(nullptr);
    }
    return array->ElementType();
  }
  return CompilerType();
}

CompilerType MotokoASTContext::GetCanonicalType(lldb::opaque_compiler_type_t type) {
  MotokoTypedef *t = static_cast<MotokoType *>(type)->AsTypedef();
  if (t)
    return t->UnderlyingType();
  return CompilerType(this, type);
}

CompilerType
MotokoASTContext::GetFullyUnqualifiedType(lldb::opaque_compiler_type_t type) {
  return CompilerType(this, type);
}

// Returns -1 if this isn't a function or if the function doesn't have a
// prototype.
// Returns a value >= 0 if there is a prototype.
int MotokoASTContext::GetFunctionArgumentCount(lldb::opaque_compiler_type_t type) {
  return GetNumberOfFunctionArguments(type);
}

CompilerType
MotokoASTContext::GetFunctionArgumentTypeAtIndex(lldb::opaque_compiler_type_t type,
                                               size_t idx) {
  return GetFunctionArgumentAtIndex(type, idx);
}

CompilerType
MotokoASTContext::GetFunctionReturnType(lldb::opaque_compiler_type_t type) {
  if (type) {
    MotokoFunction *t = static_cast<MotokoType *>(type)->AsFunction();
    if (t) {
      return t->ReturnType();
    }
  }
  return CompilerType();
}

size_t MotokoASTContext::GetNumMemberFunctions(lldb::opaque_compiler_type_t type) {
  return 0;
}

TypeMemberFunctionImpl
MotokoASTContext::GetMemberFunctionAtIndex(lldb::opaque_compiler_type_t type,
                                         size_t idx) {
  return TypeMemberFunctionImpl();
}

CompilerType
MotokoASTContext::GetNonReferenceType(lldb::opaque_compiler_type_t type) {
  return CompilerType(this, type);
}

CompilerType MotokoASTContext::GetPointeeType(lldb::opaque_compiler_type_t type) {
  if (!type)
    return CompilerType();
  MotokoPointer *p = static_cast<MotokoType *>(type)->AsPointer();
  if (p)
    return p->PointeeType();
  return CompilerType();
}

CompilerType MotokoASTContext::GetPointerType(lldb::opaque_compiler_type_t type) {
  ConstString type_name = GetTypeName(type);
  // Arbitrarily look for a raw pointer here.
  ConstString pointer_name(std::string("*mut ") + type_name.GetCString());
  return CreatePointerType(pointer_name, CompilerType(this, type), m_pointer_byte_size);
}

// If the current object represents a typedef type, get the underlying type
CompilerType MotokoASTContext::GetTypedefedType(lldb::opaque_compiler_type_t type) {
  if (type) {
    MotokoTypedef *t = static_cast<MotokoType *>(type)->AsTypedef();
    if (t)
      return t->UnderlyingType();
  }
  return CompilerType();
}

//----------------------------------------------------------------------
// Create related types using the current type's AST
//----------------------------------------------------------------------
CompilerType MotokoASTContext::GetBasicTypeFromAST(lldb::BasicType basic_type) {
  return CompilerType();
}

CompilerType
MotokoASTContext::GetBuiltinTypeForEncodingAndBitSize(lldb::Encoding encoding,
                                                    size_t bit_size) {
  return CompilerType();
}

//----------------------------------------------------------------------
// Exploring the type
//----------------------------------------------------------------------

const llvm::fltSemantics &
MotokoASTContext::GetFloatTypeSemantics(size_t byte_size) {
  /*
  clang::ASTContext &ast = getASTContext();
  const size_t bit_size = byte_size * 8;
  if (bit_size == ast.getTypeSize(ast.FloatTy))
    return ast.getFloatTypeSemantics(ast.FloatTy);
  else if (bit_size == ast.getTypeSize(ast.DoubleTy))
    return ast.getFloatTypeSemantics(ast.DoubleTy);
  else if (bit_size == ast.getTypeSize(ast.LongDoubleTy))
    return ast.getFloatTypeSemantics(ast.LongDoubleTy);
  else if (bit_size == ast.getTypeSize(ast.HalfTy))
  return ast.getFloatTypeSemantics(ast.HalfTy);*/
  return llvm::APFloatBase::Bogus();
}

llvm::Optional<uint64_t>
MotokoASTContext::GetBitSize(lldb::opaque_compiler_type_t type,
			   ExecutionContextScope *exe_scope) {
  if (!type)
    return 0;
  MotokoType *t = static_cast<MotokoType *>(type);
  if (llvm::Optional<uint64_t> bit_size = t->ByteSize())
    return *bit_size * 8;
  else return llvm::None;
}

lldb::Encoding MotokoASTContext::GetEncoding(lldb::opaque_compiler_type_t type,
                                           uint64_t &count) {
  count = 1;
  bool is_signed;
  if (IsIntegerType(type, is_signed)) {
    return is_signed ? eEncodingSint : eEncodingUint;
  }
  if (IsBooleanType(type)) {
    return eEncodingUint;
  }
  bool is_complex;
  uint32_t complex_count;
  if (IsFloatingPointType(type, complex_count, is_complex)) {
    count = complex_count;
    return eEncodingIEEE754;
  }
  if (IsPointerType(type))
    return eEncodingUint;
  return eEncodingInvalid;
}

lldb::Format MotokoASTContext::GetFormat(lldb::opaque_compiler_type_t type) {
  if (!type)
    return eFormatDefault;
  return static_cast<MotokoType *>(type)->Format();
}

llvm::Optional<size_t>
MotokoASTContext::GetTypeBitAlign(lldb::opaque_compiler_type_t type,
				ExecutionContextScope*) {
  return 0;
}

uint32_t MotokoASTContext::GetNumChildren(lldb::opaque_compiler_type_t type,
                                        bool omit_empty_base_classes,
					const ExecutionContext *exe_ctx) {
  if (!type)
    return 0;

  MotokoType *t = static_cast<MotokoType *>(type);
  uint32_t result = 0;
  if (MotokoPointer *ptr = t->AsPointer()) {
    result = ptr->PointeeType().GetNumChildren(omit_empty_base_classes, exe_ctx);
    // If the pointee is not an aggregate, return 1 because the
    // pointer has a child.  Not totally sure this makes sense.
    if (result == 0)
      result = 1;
  } else if (MotokoArray *array = t->AsArray()) {
    result = array->Length();
  } else if (MotokoTypedef *typ = t->AsTypedef()) {
    result = typ->UnderlyingType().GetNumChildren(omit_empty_base_classes, exe_ctx);
  } else if (MotokoAggregateBase *agg = t->AsAggregate()) {
    result = agg->FieldCount();
  }

  return result;
}

uint32_t MotokoASTContext::GetNumFields(lldb::opaque_compiler_type_t type) {
  if (!type)
    return 0;
  MotokoType *t = static_cast<MotokoType *>(type);
  if (MotokoTypedef *tdef = t->AsTypedef())
    return tdef->UnderlyingType().GetNumFields();
  if (MotokoAggregateBase *a = t->AsAggregate())
    return a->FieldCount();
  return 0;
}

CompilerType MotokoASTContext::GetFieldAtIndex(lldb::opaque_compiler_type_t type,
                                             size_t idx, std::string &name,
                                             uint64_t *bit_offset_ptr,
                                             uint32_t *bitfield_bit_size_ptr,
                                             bool *is_bitfield_ptr) {
  if (bit_offset_ptr)
    *bit_offset_ptr = 0;
  if (bitfield_bit_size_ptr)
    *bitfield_bit_size_ptr = 0;
  if (is_bitfield_ptr)
    *is_bitfield_ptr = false;

  if (!type || !GetCompleteType(type))
    return CompilerType();

  MotokoType *t = static_cast<MotokoType *>(type);
  if (MotokoTypedef *typ = t->AsTypedef())
    return typ->UnderlyingType().GetFieldAtIndex(
        idx, name, bit_offset_ptr, bitfield_bit_size_ptr, is_bitfield_ptr);

  if (MotokoAggregateBase *s = t->AsAggregate()) {
    const auto *field = s->FieldAt(idx);
    if (field) {
      name = field->m_name.GetStringRef();
      if (bit_offset_ptr)
        *bit_offset_ptr = field->m_offset * 8;
      return field->m_type;
    }
  }
  return CompilerType();
}

CompilerType MotokoASTContext::GetChildCompilerTypeAtIndex(
    lldb::opaque_compiler_type_t type, ExecutionContext *exe_ctx, size_t idx,
    bool transparent_pointers, bool omit_empty_base_classes,
    bool ignore_array_bounds, std::string &child_name,
    uint32_t &child_byte_size, int32_t &child_byte_offset,
    uint32_t &child_bitfield_bit_size, uint32_t &child_bitfield_bit_offset,
    bool &child_is_base_class, bool &child_is_deref_of_parent,
    ValueObject *valobj, uint64_t &language_flags) {
  child_name.clear();
  child_byte_size = 0;
  child_byte_offset = 0;
  child_bitfield_bit_size = 0;
  child_bitfield_bit_offset = 0;
  child_is_base_class = false;
  child_is_deref_of_parent = false;
  language_flags = 0;

  if (!type || !GetCompleteType(type))
    return CompilerType();

  MotokoType *t = static_cast<MotokoType *>(type);
  if (t->AsAggregate()) {
    uint64_t bit_offset;
    CompilerType ret =
        GetFieldAtIndex(type, idx, child_name, &bit_offset, nullptr, nullptr);
    child_byte_size = *ret.GetByteSize(
        exe_ctx ? exe_ctx->GetBestExecutionContextScope() : nullptr);
    child_byte_offset = bit_offset / 8;
    return ret;
  } else if (MotokoPointer *ptr = t->AsPointer()) {
    CompilerType pointee = ptr->PointeeType();
    if (!pointee.IsValid() || pointee.IsVoidType())
      return CompilerType();
    if (transparent_pointers && pointee.IsAggregateType()) {
      bool tmp_child_is_deref_of_parent = false;
      return pointee.GetChildCompilerTypeAtIndex(
          exe_ctx, idx, transparent_pointers, omit_empty_base_classes,
          ignore_array_bounds, child_name, child_byte_size, child_byte_offset,
          child_bitfield_bit_size, child_bitfield_bit_offset,
          child_is_base_class, tmp_child_is_deref_of_parent, valobj,
          language_flags);
    } else {
      child_is_deref_of_parent = true;
      const char *parent_name = valobj ? valobj->GetName().GetCString() : NULL;
      if (parent_name) {
        child_name.assign(1, '*');
        child_name += parent_name;
      }

      // We have a pointer to an simple type
      if (idx == 0 && pointee.GetCompleteType()) {
        child_byte_size = *pointee.GetByteSize(
            exe_ctx ? exe_ctx->GetBestExecutionContextScope() : NULL);
        child_byte_offset = 0;
        return pointee;
      }
    }
  } else if (MotokoArray *a = t->AsArray()) {
    if (ignore_array_bounds || idx < a->Length()) {
      CompilerType element_type = a->ElementType();
      if (element_type.GetCompleteType()) {
        char element_name[64];
        ::snprintf(element_name, sizeof(element_name), "[%zu]", idx);
        child_name.assign(element_name);
        child_byte_size = *element_type.GetByteSize(
            exe_ctx ? exe_ctx->GetBestExecutionContextScope() : NULL);
        child_byte_offset = (int32_t)idx * (int32_t)child_byte_size;
        return element_type;
      }
    }
  } else if (MotokoTypedef *typ = t->AsTypedef()) {
    return typ->UnderlyingType().GetChildCompilerTypeAtIndex(
        exe_ctx, idx, transparent_pointers, omit_empty_base_classes,
        ignore_array_bounds, child_name, child_byte_size, child_byte_offset,
        child_bitfield_bit_size, child_bitfield_bit_offset, child_is_base_class,
        child_is_deref_of_parent, valobj, language_flags);
  }
  return CompilerType();
}

// Lookup a child given a name. This function will match base class names
// and member member names in "clang_type" only, not descendants.
uint32_t
MotokoASTContext::GetIndexOfChildWithName(lldb::opaque_compiler_type_t type,
                                        const char *name,
                                        bool omit_empty_base_classes) {
  if (!type || !GetCompleteType(type))
    return UINT_MAX;

  MotokoType *t = static_cast<MotokoType *>(type);
  if (MotokoAggregateBase *agg = t->AsAggregate()) {
    for (uint32_t i = 0; i < agg->FieldCount(); ++i) {
      const MotokoAggregateBase::Field *f = agg->FieldAt(i);
      if (f->m_name.GetStringRef() == name)
        return i;
    }
  } else if (MotokoPointer *typ = t->AsPointer()) {
    return typ->PointeeType().GetIndexOfChildWithName(name, omit_empty_base_classes);
  }
  return UINT_MAX;
}

// Lookup a child member given a name. This function will match member names
// only and will descend into "clang_type" children in search for the first
// member in this class, or any base class that matches "name".
// TODO: Return all matches for a given name by returning a
// vector<vector<uint32_t>>
// so we catch all names that match a given child name, not just the first.
size_t MotokoASTContext::GetIndexOfChildMemberWithName(
    lldb::opaque_compiler_type_t type, const char *name,
    bool omit_empty_base_classes, std::vector<uint32_t> &child_indexes) {
  uint32_t index = GetIndexOfChildWithName(type, name, omit_empty_base_classes);
  if (index == UINT_MAX)
    return 0;
  child_indexes.push_back(index);
  return 1;
}

// Converts "s" to a floating point value and place resulting floating
// point bytes in the "dst" buffer.
/*size_t
MotokoASTContext::ConvertStringToFloatValue(lldb::opaque_compiler_type_t type,
                                        const char *s, uint8_t *dst,
                                        size_t dst_size) {
  assert(false);
  return 0;
  }*/

//----------------------------------------------------------------------
// Dumping types
//----------------------------------------------------------------------
#define DEPTH_INCREMENT 2

#ifndef NDEBUG
LLVM_DUMP_METHOD void
MotokoASTContext::dump(lldb::opaque_compiler_type_t type) const {
  if (!type)
    return;
  //clang::QualType qual_type(GetQualType(type));
  //qual_type.dump();
}
#endif

void MotokoASTContext::DumpValue(lldb::opaque_compiler_type_t type,
                               ExecutionContext *exe_ctx, Stream *s,
                               lldb::Format format, const DataExtractor &data,
                               lldb::offset_t data_byte_offset,
                               size_t data_byte_size, uint32_t bitfield_bit_size,
                               uint32_t bitfield_bit_offset, bool show_types,
                               bool show_summary, bool verbose, uint32_t depth) {
  // This doesn't seem to be needed.
  assert(false && "Not implemented");
}

bool MotokoASTContext::DumpTypeValue(lldb::opaque_compiler_type_t type, Stream *s,
                                   lldb::Format format, const DataExtractor &data,
                                   lldb::offset_t byte_offset, size_t byte_size,
                                   uint32_t bitfield_bit_size,
                                   uint32_t bitfield_bit_offset,
                                   ExecutionContextScope *exe_scope) {
  if (!type)
    return false;
  if (IsAggregateType(type)) {
    return false;
  } else {
    MotokoType *t = static_cast<MotokoType *>(type);
    if (MotokoTypedef *typ = t->AsTypedef()) {
      CompilerType typedef_compiler_type = typ->UnderlyingType();
      if (format == eFormatDefault)
        format = typedef_compiler_type.GetFormat();
      uint64_t typedef_byte_size = *typedef_compiler_type.GetByteSize(exe_scope);

      return typedef_compiler_type.DumpTypeValue(
          s,
          format,            // The format with which to display the element
          data,              // Data buffer containing all bytes for this type
          byte_offset,       // Offset into "data" where to grab value from
          typedef_byte_size, // Size of this type in bytes
          bitfield_bit_size, // Size in bits of a bitfield value, if zero don't
                             // treat as a bitfield
          bitfield_bit_offset, // Offset in bits of a bitfield value if
                               // bitfield_bit_size != 0
          exe_scope);
    }

    if (format == eFormatEnum || format == eFormatDefault) {
      if (auto clike = t->AsCLikeEnum()) {
        uint32_t value;
        if (clike->IsSigned()) {
	  assert(false);
          int64_t svalue = data.GetMaxS64Bitfield(&byte_offset, byte_size,
                                                  bitfield_bit_size,
                                                  bitfield_bit_offset);
          value = uint32_t(svalue);
        } else {
          value = data.GetMaxU64Bitfield(&byte_offset, byte_size,
                                         bitfield_bit_size,
                                         bitfield_bit_offset);
        }

        std::string name;
        if (clike->FindName(value, name)) {
          s->Printf("%s::%s", clike->Name().AsCString(), name.c_str());
        } else {
          // If the value couldn't be found, then something went wrong
          // we should inform the user.
          s->Printf("(invalid enum value) %" PRIu32, value);
        }
        return true;
      }
    } else if (format == eFormatUnicode32) {
      if (MotokoIntegral *intlike = t->AsInteger()) {
        if (intlike->IsCharType()) {
          uint64_t value = data.GetMaxU64Bitfield(&byte_offset, byte_size,
                                                  bitfield_bit_size,
                                                  bitfield_bit_offset);
          switch (value) {
          case '\n':
            s->PutCString("'\\n'");
            break;
          case '\r':
            s->PutCString("'\\r'");
            break;
          case '\t':
            s->PutCString("'\\t'");
            break;
          case '\\':
            s->PutCString("'\\\\'");
            break;
          case '\0':
            s->PutCString("'\\0'");
            break;
          case '\'':
            s->PutCString("'\\''");
            break;

          default:
            if (value < 128 && isprint(value)) {
              s->Printf("'%c'", char(value));
            } else {
              s->Printf("'\\u{%x}'", unsigned(value));
            }
            break;
          }

          return true;
        }
      }
    }

    uint32_t item_count = 1;
    switch (format) {
    default:
    case eFormatBoolean:
    case eFormatBinary:
    case eFormatComplex:
    case eFormatCString:
    case eFormatDecimal:
    case eFormatEnum:
    case eFormatHex:
    case eFormatHexUppercase:
    case eFormatFloat:
    case eFormatOctal:
    case eFormatOSType:
    case eFormatUnsigned:
    case eFormatPointer:
    case eFormatVectorOfChar:
    case eFormatVectorOfSInt8:
    case eFormatVectorOfUInt8:
    case eFormatVectorOfSInt16:
    case eFormatVectorOfUInt16:
    case eFormatVectorOfSInt32:
    case eFormatVectorOfUInt32:
    case eFormatVectorOfSInt64:
    case eFormatVectorOfUInt64:
    case eFormatVectorOfFloat32:
    case eFormatVectorOfFloat64:
    case eFormatVectorOfUInt128:
      break;

    case eFormatChar:
    case eFormatCharPrintable:
    case eFormatCharArray:
    case eFormatBytes:
    case eFormatBytesWithASCII:
      item_count = byte_size;
      byte_size = 1;
      break;

    case eFormatUnicode16:
      item_count = byte_size / 2;
      byte_size = 2;
      break;

    case eFormatUnicode32:
      item_count = byte_size / 4;
      byte_size = 4;
      break;
    }
    return DumpDataExtractor(data, s, byte_offset, format, byte_size, item_count,
                             UINT32_MAX, LLDB_INVALID_ADDRESS, bitfield_bit_size,
                             bitfield_bit_offset, exe_scope);
  }
  return 0;
}

void MotokoASTContext::DumpSummary(lldb::opaque_compiler_type_t type,
                                 ExecutionContext *exe_ctx, Stream *s,
                                 const DataExtractor &data,
                                 lldb::offset_t data_offset,
                                 size_t data_byte_size) {
  // Apparently there is nothing to do here.
}

void MotokoASTContext::DumpTypeDescription(lldb::opaque_compiler_type_t type) {
  // Dump to stdout
  StreamFile s(stdout, false);
  DumpTypeDescription(type, &s);
}

void MotokoASTContext::DumpTypeDescription(lldb::opaque_compiler_type_t type, Stream *s) {
  if (!type)
    return;
  ConstString name = GetTypeName(type);
  MotokoType *t = static_cast<MotokoType *>(type);

  if (MotokoAggregateBase *agg = t->AsAggregate()) {
    s->PutCString(agg->Tag());
    const char *name = agg->TagName();
    s->PutCString(name);
    if (*name) {
      s->PutCString(" ");
    }
    s->PutCString(agg->Opener());
    if (agg->FieldCount() == 0) {
      s->PutCString(agg->Closer());
      return;
    }
    s->IndentMore();
    // A trailing comma looks weird for tuples, so we keep track and
    // don't emit it.
    bool first = true;
    for (auto &&field : *agg) {
      if (!first) {
        s->PutChar(',');
      }
      first = false;
      s->PutChar('\n');
      s->Indent();
      if (!field.m_name.IsEmpty()) {
        s->PutCString(field.m_name.AsCString());
        s->PutCString(": ");
      }
      s->PutCString(field.m_type.GetTypeName().AsCString());
    }
    s->IndentLess();
    s->PutChar('\n');
    s->Indent(agg->Closer());
    return;
  }

  s->PutCString(name.AsCString());
}

CompilerType MotokoASTContext::CacheType(MotokoType *new_type) {
  m_types.insert(std::unique_ptr<MotokoType>(new_type));
  return CompilerType(this, new_type);
}

CompilerType MotokoASTContext::CreateBoolType(const lldb_private::ConstString &name) {
  MotokoType *type = new MotokoBool(name);
  return CacheType(type);
}

CompilerType MotokoASTContext::CreateIntegralType(const lldb_private::ConstString &name,
                                                bool is_signed,
                                                uint64_t byte_size,
                                                bool is_char_type) {
  MotokoType *type = new MotokoIntegral(name, is_signed, byte_size, is_char_type);
  return CacheType(type);
}

CompilerType MotokoASTContext::CreateIntrinsicIntegralType(bool is_signed, uint64_t byte_size) {
  char name[100];
  snprintf(name, sizeof(name), "%s%d", is_signed ? "i" : "u", int(byte_size * 8));

  ConstString cname(name);
  return CreateIntegralType(cname, is_signed, byte_size);
}

CompilerType MotokoASTContext::CreateCharType() {
  ConstString cname("char");
  return CreateIntegralType(cname, false, 4, true);
}

CompilerType MotokoASTContext::CreateFloatType(const lldb_private::ConstString &name,
                                             uint64_t byte_size) {
  MotokoType *type = new MotokoFloat(name, byte_size);
  return CacheType(type);
}

CompilerType MotokoASTContext::CreateArrayType(const CompilerType &element_type,
                                             uint64_t length) {
  std::string name = std::string("[") + element_type.GetTypeName().AsCString();
  if (length != 0) {
    name = name + "; " + std::to_string(length);
  }
  name += "]";
  ConstString newname(name);

  MotokoType *type = new MotokoArray(newname, length, element_type);
  return CacheType(type);
}

CompilerType MotokoASTContext::CreateTypedefType(const ConstString &name, CompilerType impl) {
  MotokoType *type = new MotokoTypedef(name, impl);
  return CacheType(type);
}

CompilerType
MotokoASTContext::CreateStructType(const lldb_private::ConstString &name, uint32_t byte_size,
                                 bool has_discriminant) {
  MotokoType *type = new MotokoStruct(name, byte_size, has_discriminant);
  return CacheType(type);
}

CompilerType
MotokoASTContext::CreateTupleType(const lldb_private::ConstString &name, uint32_t byte_size,
                                bool has_discriminant) {
  MotokoType *type = new MotokoTuple(name, byte_size, has_discriminant);
  return CacheType(type);
}

CompilerType
MotokoASTContext::CreateUnionType(const lldb_private::ConstString &name, uint32_t byte_size) {
  MotokoType *type = new MotokoUnion(name, byte_size);
  return CacheType(type);
}

CompilerType
MotokoASTContext::CreatePointerType(const lldb_private::ConstString &name,
                                  const CompilerType &pointee_type,
                                  uint32_t byte_size) {
  MotokoType *type = new MotokoPointer(name, pointee_type, byte_size);
  return CacheType(type);
}

void MotokoASTContext::AddFieldToStruct(const CompilerType &struct_type,
                                      const lldb_private::ConstString &name,
                                      const CompilerType &field_type,
                                      uint32_t byte_offset,
                                      bool is_default, uint64_t discriminant) {
  if (!struct_type)
    return;
  MotokoASTContext *ast =
      llvm::dyn_cast_or_null<MotokoASTContext>(struct_type.GetTypeSystem());
  if (!ast)
    return;
  MotokoType *type = static_cast<MotokoType *>(struct_type.GetOpaqueQualType());
  if (MotokoAggregateBase *a = type->AsAggregate()) {
    a->AddField(name, field_type, byte_offset);
    if (MotokoEnum *e = type->AsEnum()) {
      e->RecordDiscriminant(is_default, discriminant);
    }
  }
}

CompilerType
MotokoASTContext::CreateFunctionType(const lldb_private::ConstString &name,
                                   const CompilerType &return_type,
                                   const std::vector<CompilerType> &&params,
                                   const std::vector<CompilerType> &&template_params) {
  MotokoType *type = new MotokoFunction(name, m_pointer_byte_size, return_type, std::move(params),
				    std::move(template_params));
  return CacheType(type);
}

CompilerType
MotokoASTContext::CreateVoidType() {
  ConstString name("()");
  MotokoType *type = new MotokoTuple(name, 0, false);
  return CacheType(type);
}

CompilerType
MotokoASTContext::CreateEnumType(const lldb_private::ConstString &name,
                               uint64_t byte_size, uint32_t discr_offset,
                               uint32_t discr_byte_size) {
  MotokoType *type = new MotokoEnum(name, byte_size, discr_offset, discr_byte_size);
  return CacheType(type);
}

CompilerType
MotokoASTContext::CreateCLikeEnumType(const lldb_private::ConstString &name,
                                    const CompilerType &underlying_type,
                                    std::map<uint32_t, std::string> &&values) {
  MotokoType *type = new MotokoCLikeEnum(name, underlying_type, std::move(values));
  return CacheType(type);
}

bool
MotokoASTContext::IsTupleType(const CompilerType &type) {
  if (!type)
    return false;
  MotokoASTContext *ast = llvm::dyn_cast_or_null<MotokoASTContext>(type.GetTypeSystem());
  if (!ast)
    return false;
  MotokoType *rtype = static_cast<MotokoType *>(type.GetOpaqueQualType());
  return bool(rtype->AsTuple());
}

bool
MotokoASTContext::TypeHasDiscriminant(const CompilerType &type) {
  if (!type)
    return false;
  MotokoASTContext *ast = llvm::dyn_cast_or_null<MotokoASTContext>(type.GetTypeSystem());
  if (!ast)
    return false;
  MotokoType *rtype = static_cast<MotokoType *>(type.GetOpaqueQualType());
  if (MotokoAggregateBase *a = rtype->AsAggregate())
    return a->HasDiscriminant();
  return false;
}

bool
MotokoASTContext::GetEnumDiscriminantLocation(const CompilerType &type, uint64_t &discr_offset,
                                            uint64_t &discr_byte_size) {
  if (!type)
    return false;
  MotokoASTContext *ast = llvm::dyn_cast_or_null<MotokoASTContext>(type.GetTypeSystem());
  if (!ast)
    return false;
  MotokoType *rtype = static_cast<MotokoType *>(type.GetOpaqueQualType());
  if (MotokoEnum *e = rtype->AsEnum()) {
    e->GetDiscriminantLocation(discr_offset, discr_byte_size);
    return true;
  }
  return false;
}

CompilerType
MotokoASTContext::FindEnumVariant(const CompilerType &type, uint64_t discriminant) {
  if (!type)
    return CompilerType();
  MotokoASTContext *ast = llvm::dyn_cast_or_null<MotokoASTContext>(type.GetTypeSystem());
  if (!ast)
    return CompilerType();
  MotokoType *rtype = static_cast<MotokoType *>(type.GetOpaqueQualType());
  if (MotokoEnum *e = rtype->AsEnum()) {
    return e->FindEnumVariant(discriminant);
  }
  return CompilerType();
}

void
MotokoASTContext::FinishAggregateInitialization(const CompilerType &type) {
  if (!type)
    return;
  MotokoASTContext *ast = llvm::dyn_cast_or_null<MotokoASTContext>(type.GetTypeSystem());
  if (!ast)
    return;
  MotokoType *rtype = static_cast<MotokoType *>(type.GetOpaqueQualType());
  if (MotokoAggregateBase *a = rtype->AsAggregate())
    a->FinishInitialization();
}

DWARFASTParser *MotokoASTContext::GetDWARFParser() {
  if (!m_dwarf_ast_parser_ap)
    m_dwarf_ast_parser_ap.reset(new DWARFASTParserMotoko(*this));
  return m_dwarf_ast_parser_ap.get();
}

UserExpression *MotokoASTContextForExpr::GetUserExpression(
    llvm::StringRef expr, llvm::StringRef prefix, lldb::LanguageType language,
    Expression::ResultType desired_type,
    const EvaluateExpressionOptions &options,
    ValueObject *ctx_obj) {
  TargetSP target = m_target_wp.lock();
  if (target)
    return new RustUserExpression(*target, expr, prefix, language, desired_type,
                                  options);
  //return new MotokoUserExpression(*target, expr, prefix, language, desired_type,
  //                                options);
  return nullptr;
}

ConstString MotokoASTContext::DeclGetName(void *opaque_decl) {
  MotokoDecl *dc = (MotokoDecl *) opaque_decl;
  return dc->Name();
}

ConstString MotokoASTContext::DeclGetMangledName(void *opaque_decl) {
  MotokoDecl *dc = (MotokoDecl *) opaque_decl;
  return dc->MangledName();
}

CompilerType MotokoASTContext::GetTypeForDecl(void *opaque_decl) {
  if (!opaque_decl)
    return CompilerType();

  /*clang::Decl *decl = static_cast<clang::Decl *>(opaque_decl);
  if (auto *named_decl = llvm::dyn_cast<clang::NamedDecl>(decl))
    return GetTypeForDecl(named_decl);*/
  return CompilerType();
}

CompilerDeclContext MotokoASTContext::DeclGetDeclContext(void *opaque_decl) {
  MotokoDecl *dc = (MotokoDecl *) opaque_decl;
  return CompilerDeclContext(this, dc->Context());
}

ConstString MotokoASTContext::DeclContextGetName(void *opaque_decl_ctx) {
  MotokoDeclContext *dc = (MotokoDeclContext *) opaque_decl_ctx;
  return dc->Name();
}

ConstString MotokoASTContext::DeclContextGetScopeQualifiedName(void *opaque_decl_ctx) {
  MotokoDeclContext *dc = (MotokoDeclContext *) opaque_decl_ctx;
  return dc->QualifiedName();
}

bool MotokoASTContext::DeclContextIsContainedInLookup(
    void *opaque_decl_ctx, void *other_opaque_decl_ctx) {
  /*
  auto *decl_ctx = (clang::DeclContext *)opaque_decl_ctx;
  auto *other = (clang::DeclContext *)other_opaque_decl_ctx;

  do {
    // A decl context always includes its own contents in its lookup.
    if (decl_ctx == other)
      return true;

    // If we have an inline namespace, then the lookup of the parent context
    // also includes the inline namespace contents.
  } while (other->isInlineNamespace() && (other = other->getParent()));
  */

  return false;
}



/*
bool MotokoASTContext::DeclContextIsStructUnionOrClass(void *opaque_decl_ctx) {
  // This is not actually correct -- for example an enum arm is nested
  // in its containing enum -- but as far as I can tell this result
  // doesn't matter for Motoko.
  return false;
  }*/

bool MotokoASTContext::DeclContextIsClassMethod(void *opaque_decl_ctx,
                                              lldb::LanguageType *language_ptr,
                                              bool *is_instance_method_ptr,
                                              ConstString *language_object_name_ptr) {
  return false;
}

std::vector<CompilerDecl>
MotokoASTContext::DeclContextFindDeclByName(void *opaque_decl_ctx, ConstString name,
                                          const bool ignore_imported_decls) {
  std::vector<CompilerDecl> result;
  SymbolFile *symbol_file = GetSymbolFile();
  if (symbol_file) {
    symbol_file->ParseDeclsForContext(CompilerDeclContext(this, opaque_decl_ctx));

    MotokoDeclContext *dc = (MotokoDeclContext *) opaque_decl_ctx;
    MotokoDeclBase *base = dc->FindByName(name);
    if (MotokoDecl *decl = base ? base->AsDecl() : nullptr) {
      result.push_back(CompilerDecl(this, decl));
    }
  }
  return result;
}

CompilerDeclContext MotokoASTContext::GetTranslationUnitDecl() {
  if (!m_tu_decl) {
    m_tu_decl.reset(new MotokoDeclContext(ConstString(""), nullptr));
  }
  return CompilerDeclContext(this, m_tu_decl.get());
}

CompilerDeclContext
MotokoASTContext::GetNamespaceDecl(CompilerDeclContext parent, const ConstString &name) {
  if (!parent)
    return CompilerDeclContext();
  MotokoASTContext *ast = llvm::dyn_cast_or_null<MotokoASTContext>(parent.GetTypeSystem());
  if (!ast)
    return CompilerDeclContext();

  MotokoDeclContext *dc = (MotokoDeclContext *) parent.GetOpaqueDeclContext();
  MotokoDeclBase *base = dc->FindByName(name);
  if (base) {
    if (MotokoDeclContext *ctx = base->AsDeclContext()) {
      return CompilerDeclContext(this, ctx);
    }
  }

  MotokoDeclContext *new_ns = new MotokoDeclContext(name, dc);
  dc->AddItem(std::unique_ptr<MotokoDeclBase>(new_ns));
  return CompilerDeclContext(this, new_ns);
}

CompilerDeclContext
MotokoASTContext::GetDeclContextDeclContext(CompilerDeclContext child) {
  if (!child)
    return CompilerDeclContext();
  MotokoASTContext *ast = llvm::dyn_cast_or_null<MotokoASTContext>(child.GetTypeSystem());
  if (!ast)
    return CompilerDeclContext();

  MotokoDeclContext *dc = (MotokoDeclContext *) child.GetOpaqueDeclContext();
  return CompilerDeclContext(this, dc->Context());
}

CompilerDecl MotokoASTContext::GetDecl(CompilerDeclContext parent, const ConstString &name,
                                     const ConstString &mangled) {
  if (!parent)
    return CompilerDecl();
  MotokoASTContext *ast = llvm::dyn_cast_or_null<MotokoASTContext>(parent.GetTypeSystem());
  if (!ast)
    return CompilerDecl();

  MotokoDeclContext *dc = (MotokoDeclContext *) parent.GetOpaqueDeclContext();
  MotokoDeclBase *base = dc->FindByName(name);
  if (base) {
    if (MotokoDecl *ctx = base->AsDecl()) {
      return CompilerDecl(this, ctx);
    }
  }

  MotokoDecl *new_ns = new MotokoDecl(name, mangled, dc);
  dc->AddItem(std::unique_ptr<MotokoDeclBase>(new_ns));
  return CompilerDecl(this, new_ns);
}

bool MotokoASTContext::GetCABITypeDeclaration(CompilerType type, const std::string &varname,
                                            MotokoASTContext::TypeNameMap *name_map,
                                            std::string *result) {
  if (!type)
    return false;
  MotokoASTContext *ast = llvm::dyn_cast_or_null<MotokoASTContext>(type.GetTypeSystem());
  if (!ast)
    return false;
  MotokoType *rtype = static_cast<MotokoType *>(type.GetOpaqueQualType());
  *result = rtype->GetCABITypeDeclaration(name_map, varname);
  return true;
}

CompilerType MotokoASTContext::GetTypeTemplateArgument(lldb::opaque_compiler_type_t type,
						     size_t idx) {
  if (type) {
    MotokoType *t = static_cast<MotokoType *>(type);
    if (MotokoAggregateBase *a = t->AsAggregate()) {
      return a->GetTypeTemplateArgument(idx);
    } else if (MotokoFunction *f = t->AsFunction()) {
      return f->GetTypeTemplateArgument(idx);
    }
  }
  return CompilerType();
}

size_t MotokoASTContext::GetNumTemplateArguments(lldb::opaque_compiler_type_t type) {
  if (type) {
    MotokoType *t = static_cast<MotokoType *>(type);
    if (MotokoAggregateBase *a = t->AsAggregate()) {
      return a->GetNumTemplateArguments();
    } else if (MotokoFunction *f = t->AsFunction()) {
      return f->GetNumTemplateArguments();
    }
  }
  return 0;
}

void MotokoASTContext::AddTemplateParameter(const CompilerType &type, const CompilerType &param) {
  if (!type)
    return;
  MotokoASTContext *ast = llvm::dyn_cast_or_null<MotokoASTContext>(type.GetTypeSystem());
  if (!ast)
    return;
  MotokoType *t = static_cast<MotokoType *>(type.GetOpaqueQualType());
  if (MotokoAggregateBase *a = t->AsAggregate()) {
    a->AddTemplateParameter(param);
  }
}
