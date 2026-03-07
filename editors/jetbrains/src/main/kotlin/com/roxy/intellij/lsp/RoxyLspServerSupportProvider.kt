package com.roxy.intellij.lsp

import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.notification.NotificationGroupManager
import com.intellij.notification.NotificationType
import com.intellij.openapi.project.Project
import com.intellij.openapi.vfs.VirtualFile
import com.intellij.platform.lsp.api.LspServerSupportProvider
import com.intellij.platform.lsp.api.LspServerSupportProvider.LspServerStarter
import com.intellij.platform.lsp.api.ProjectWideLspServerDescriptor

class RoxyLspServerSupportProvider : LspServerSupportProvider {
    override fun fileOpened(
        project: Project,
        file: VirtualFile,
        serverStarter: LspServerStarter
    ) {
        if (file.extension == "roxy") {
            val serverPath = resolveServerPath()
            if (serverPath != null) {
                serverStarter.ensureServerStarted(RoxyLspServerDescriptor(project, serverPath))
            } else {
                NotificationGroupManager.getInstance()
                    .getNotificationGroup("Roxy LSP")
                    .createNotification(
                        "Roxy LSP server not found. Configure the path in Settings | Tools | Roxy Language.",
                        NotificationType.WARNING
                    )
                    .notify(project)
            }
        }
    }

    private fun resolveServerPath(): String? {
        val configured = RoxyLspSettings.getInstance().lspServerPath
        if (configured.isNotBlank() && java.io.File(configured).canExecute()) {
            return configured
        }

        // Try to find roxy_lsp_server in PATH
        val pathDirs = System.getenv("PATH")?.split(java.io.File.pathSeparator) ?: return null
        val names = listOf("roxy_lsp_server", "roxy_lsp_server.exe")
        for (dir in pathDirs) {
            for (name in names) {
                val candidate = java.io.File(dir, name)
                if (candidate.canExecute()) {
                    return candidate.absolutePath
                }
            }
        }
        return null
    }
}

private class RoxyLspServerDescriptor(
    project: Project,
    private val serverPath: String
) : ProjectWideLspServerDescriptor(project, "Roxy") {

    override fun isSupportedFile(file: VirtualFile): Boolean {
        return file.extension == "roxy"
    }

    override fun createCommandLine(): GeneralCommandLine {
        return GeneralCommandLine(serverPath)
    }
}
