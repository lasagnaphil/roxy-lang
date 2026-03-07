package com.roxy.intellij

import com.intellij.lang.Language

object RoxyLanguage : Language("Roxy") {
    private fun readResolve(): Any = RoxyLanguage
}
