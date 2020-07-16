//===-- RustLanguageRuntime.cpp ---------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "RustLanguageRuntime.h"

#include "lldb/Core/PluginManager.h"
#include "lldb/Symbol/RustASTContext.h"
#include "lldb/Symbol/Symbol.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Symbol/SymbolFile.h"
#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Target/SectionLoadList.h"
#include "lldb/Target/Target.h"
#include "llvm/ADT/StringRef.h"

using namespace lldb;
using namespace lldb_private;

RustLanguageRuntime::RustLanguageRuntime(Process *process)
    : LanguageRuntime(process)
{
}

LanguageRuntime *
RustLanguageRuntime::CreateInstance(Process *process,
                                    lldb::LanguageType language) {
  if (language == eLanguageTypeRust)
    return new RustLanguageRuntime(process);
  return nullptr;
}

void RustLanguageRuntime::Initialize() {
  PluginManager::RegisterPlugin(GetPluginNameStatic(), "Rust language runtime",
                                CreateInstance);
}

void RustLanguageRuntime::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

lldb_private::ConstString RustLanguageRuntime::GetPluginNameStatic() {
  static ConstString g_name("rust");
  return g_name;
}

lldb_private::ConstString RustLanguageRuntime::GetPluginName() {
  return GetPluginNameStatic();
}

uint32_t RustLanguageRuntime::GetPluginVersion() {
  return 1;
}

bool RustLanguageRuntime::CouldHaveDynamicValue(ValueObject &in_value) {
  return in_value.GetCompilerType().IsPossibleDynamicType(nullptr, false, false);
}

bool RustLanguageRuntime::GetDynamicTypeAndAddress(
    ValueObject &in_value, lldb::DynamicValueType use_dynamic,
    TypeAndOrName &class_type_or_name, Address &dynamic_address,
    Value::ValueType &value_type) {
  class_type_or_name.Clear();
  value_type = Value::ValueType::eValueTypeScalar;

  CompilerType type = in_value.GetCompilerType();
  RustASTContext *ast = llvm::dyn_cast_or_null<RustASTContext>(type.GetTypeSystem());

  if (!ast) {
    return false;
  }

  uint64_t discr_offset, discr_byte_size;
  if (ast->GetEnumDiscriminantLocation(type, discr_offset, discr_byte_size)) {
    lldb::addr_t original_ptr = in_value.GetAddressOf(false); // FIXME?
    if (original_ptr == LLDB_INVALID_ADDRESS) {
      return false;
    }

    ExecutionContext exe_ctx(in_value.GetExecutionContextRef());
    Process *process = exe_ctx.GetProcessPtr();
    if (process == nullptr) {
      return false;
    }

    Status error;
    uint64_t discriminant =
      process->ReadUnsignedIntegerFromMemory(original_ptr + discr_offset, discr_byte_size,
					     0, error);
    if (!error.Success()) {
      return false;
    }

    CompilerType variant_type = ast->FindEnumVariant(type, discriminant);
    class_type_or_name = TypeAndOrName(variant_type);
    // The address doesn't change.
    dynamic_address.SetLoadAddress(original_ptr, exe_ctx.GetTargetPtr());
    value_type = Value::ValueType::eValueTypeLoadAddress;

    return true;
  }

  return false;
}

TypeAndOrName
RustLanguageRuntime::FixUpDynamicType(const TypeAndOrName &type_and_or_name,
                                      ValueObject &static_value) {
  return type_and_or_name;
}

lldb::ThreadPlanSP
RustLanguageRuntime::GetStepThroughTrampolinePlan(Thread &thread,
						  bool stop_others) {
  ThreadPlanSP ret_plan_sp;
  return ret_plan_sp;

  /* GGR: Copied from CPPLanguageRuntime, others may be better!!!
  lldb::addr_t curr_pc = thread.GetRegisterContext()->GetPC();

  TargetSP target_sp(thread.CalculateTarget());

  if (target_sp->GetSectionLoadList().IsEmpty())
    return ret_plan_sp;

  Address pc_addr_resolved;
  SymbolContext sc;
  Symbol *symbol;

  if (!target_sp->GetSectionLoadList().ResolveLoadAddress(curr_pc,
                                                          pc_addr_resolved))
    return ret_plan_sp;

  target_sp->GetImages().ResolveSymbolContextForAddress(
      pc_addr_resolved, eSymbolContextEverything, sc);
  symbol = sc.symbol;

  if (symbol == nullptr)
    return ret_plan_sp;

  llvm::StringRef function_name(symbol->GetName().GetCString());

  // Handling the case where we are attempting to step into std::function.
  // The behavior will be that we will attempt to obtain the wrapped
  // callable via FindLibCppStdFunctionCallableInfo() and if we find it we
  // will return a ThreadPlanRunToAddress to the callable. Therefore we will
  // step into the wrapped callable.
  //
  bool found_expected_start_string =
      function_name.startswith("std::__1::function<");

  if (!found_expected_start_string)
    return ret_plan_sp;

  AddressRange range_of_curr_func;
  sc.GetAddressRange(eSymbolContextEverything, 0, false, range_of_curr_func);

  StackFrameSP frame = thread.GetStackFrameAtIndex(0);

  if (frame) {
    ValueObjectSP value_sp = frame->FindVariable(g_this);

    CPPLanguageRuntime::LibCppStdFunctionCallableInfo callable_info =
        FindLibCppStdFunctionCallableInfo(value_sp);

    if (callable_info.callable_case != LibCppStdFunctionCallableCase::Invalid &&
        value_sp->GetValueIsValid()) {
      // We found the std::function wrapped callable and we have its address.
      // We now create a ThreadPlan to run to the callable.
      ret_plan_sp = std::make_shared<ThreadPlanRunToAddress>(
          thread, callable_info.callable_address, stop_others);
      return ret_plan_sp;
    } else {
      // We are in std::function but we could not obtain the callable.
      // We create a ThreadPlan to keep stepping through using the address range
      // of the current function.
      ret_plan_sp = std::make_shared<ThreadPlanStepInRange>(
          thread, range_of_curr_func, sc, eOnlyThisThread, eLazyBoolYes,
          eLazyBoolYes);
      return ret_plan_sp;
    }
  }

  return ret_plan_sp;*/
}
