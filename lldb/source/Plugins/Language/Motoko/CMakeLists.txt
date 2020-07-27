add_lldb_library(lldbPluginMotokoLanguage PLUGIN
  MotokoLanguage.cpp

  LINK_LIBS
    lldbCore
    lldbDataFormatters
    lldbSymbol
    lldbTarget
  LINK_COMPONENTS
    Support
)
