package com.roxy.intellij.lsp

import com.intellij.openapi.fileChooser.FileChooserDescriptorFactory
import com.intellij.openapi.options.Configurable
import com.intellij.openapi.ui.TextFieldWithBrowseButton
import com.intellij.util.ui.FormBuilder
import javax.swing.JComponent
import javax.swing.JPanel

class RoxyLspSettingsConfigurable : Configurable {
    private var serverPathField: TextFieldWithBrowseButton? = null
    private var panel: JPanel? = null

    override fun getDisplayName(): String = "Roxy Language"

    override fun createComponent(): JComponent {
        val field = TextFieldWithBrowseButton()
        field.addBrowseFolderListener(
            "Select Roxy LSP Server",
            "Path to the roxy_lsp_server executable",
            null,
            FileChooserDescriptorFactory.createSingleFileDescriptor()
        )
        serverPathField = field

        panel = FormBuilder.createFormBuilder()
            .addLabeledComponent("LSP server path:", field)
            .addComponentFillVertically(JPanel(), 0)
            .panel

        return panel!!
    }

    override fun isModified(): Boolean {
        val settings = RoxyLspSettings.getInstance()
        return serverPathField?.text != settings.lspServerPath
    }

    override fun apply() {
        val settings = RoxyLspSettings.getInstance()
        settings.lspServerPath = serverPathField?.text ?: ""
    }

    override fun reset() {
        val settings = RoxyLspSettings.getInstance()
        serverPathField?.text = settings.lspServerPath
    }

    override fun disposeUIResources() {
        serverPathField = null
        panel = null
    }
}
