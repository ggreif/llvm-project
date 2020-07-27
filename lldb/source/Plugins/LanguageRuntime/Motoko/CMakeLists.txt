add_lldb_library(lldbPluginLanguageRuntimeMotoko PLUGIN
  MotokoLanguageRuntime.cpp

  LINK_LIBS
    lldbCore
    lldbSymbol
    lldbTarget
  LINK_COMPONENTS
    Support
  )
