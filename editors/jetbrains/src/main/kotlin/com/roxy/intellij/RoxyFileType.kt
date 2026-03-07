package com.roxy.intellij

import com.intellij.openapi.fileTypes.LanguageFileType
import javax.swing.Icon

object RoxyFileType : LanguageFileType(RoxyLanguage) {
    override fun getName(): String = "Roxy"
    override fun getDescription(): String = "Roxy language file"
    override fun getDefaultExtension(): String = "roxy"
    override fun getIcon(): Icon = RoxyIcons.FILE

    private fun readResolve(): Any = RoxyFileType
}
