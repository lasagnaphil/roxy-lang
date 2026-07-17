//! Zed extension for Roxy: launches `roxy_lsp_server`.
//!
//! Discovery mirrors the JetBrains plugin (RoxyLspServerSupportProvider): an
//! explicitly configured path wins, otherwise fall back to PATH.

use zed_extension_api::{self as zed, settings::LspSettings, LanguageServerId, Result};

const SERVER_NAME: &str = "roxy_lsp_server";

struct RoxyExtension;

impl RoxyExtension {
    fn server_binary(
        &self,
        language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<(String, Vec<String>)> {
        // 1. An explicit path from settings:
        //      "lsp": { "roxy-lsp": { "binary": { "path": "...", "arguments": [] } } }
        if let Ok(settings) = LspSettings::for_worktree(language_server_id.as_ref(), worktree) {
            if let Some(binary) = settings.binary {
                let args = binary.arguments.unwrap_or_default();
                if let Some(path) = binary.path {
                    return Ok((path, args));
                }
            }
        }

        // 2. PATH.
        if let Some(path) = worktree.which(SERVER_NAME) {
            return Ok((path, Vec::new()));
        }

        Err(format!(
            "{SERVER_NAME} not found in PATH. Build it with `ninja -C build roxy_lsp_server` and \
             either add that build directory to PATH, or set the path explicitly in settings.json:\n\
             \n  \"lsp\": {{ \"roxy-lsp\": {{ \"binary\": {{ \"path\": \"/abs/path/to/{SERVER_NAME}\" }} }} }}"
        ))
    }
}

impl zed::Extension for RoxyExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        let (command, args) = self.server_binary(language_server_id, worktree)?;
        Ok(zed::Command {
            command,
            args,
            env: worktree.shell_env(),
        })
    }
}

zed::register_extension!(RoxyExtension);
