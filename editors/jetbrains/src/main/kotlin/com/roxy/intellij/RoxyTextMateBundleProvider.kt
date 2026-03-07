package com.roxy.intellij

import com.intellij.ide.plugins.PluginManagerCore
import com.intellij.openapi.extensions.PluginId
import org.jetbrains.plugins.textmate.api.TextMateBundleProvider

class RoxyTextMateBundleProvider : TextMateBundleProvider {
    override fun getBundles(): List<TextMateBundleProvider.PluginBundle> {
        val descriptor = PluginManagerCore.getPlugin(PluginId.getId("com.roxy.intellij"))
            ?: return emptyList()
        val bundlePath = descriptor.pluginPath.resolve("textmate/roxy")
        return listOf(TextMateBundleProvider.PluginBundle("roxy", bundlePath))
    }
}
