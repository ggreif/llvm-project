//===-- MotokoLanguage.cpp ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
#include <string.h>
// C++ Includes
#include <functional>
#include <mutex>

// Other libraries and framework includes
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Threading.h"

// Project includes
#include "MotokoLanguage.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/DataFormatters/DataVisualization.h"
#include "lldb/DataFormatters/FormattersHelpers.h"
#include "lldb/Symbol/MotokoASTContext.h"
#include "lldb/Utility/ConstString.h"

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::formatters;

void MotokoLanguage::Initialize() {
  PluginManager::RegisterPlugin(GetPluginNameStatic(), "Motoko Language",
                                CreateInstance);
}

void MotokoLanguage::Terminate() {
  PluginManager::UnregisterPlugin(CreateInstance);
}

lldb_private::ConstString MotokoLanguage::GetPluginNameStatic() {
  static ConstString g_name("Motoko");
  return g_name;
}

lldb_private::ConstString MotokoLanguage::GetPluginName() {
  return GetPluginNameStatic();
}

uint32_t MotokoLanguage::GetPluginVersion() { return 1; }

Language *MotokoLanguage::CreateInstance(lldb::LanguageType language) {
  if (language == eLanguageTypeMotoko)
    return new MotokoLanguage();
  return nullptr;
}

bool MotokoLanguage::IsSourceFile(llvm::StringRef file_path) const {
  return file_path.endswith(".mo");
}
