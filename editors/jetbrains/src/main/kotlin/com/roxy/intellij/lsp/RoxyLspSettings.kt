package com.roxy.intellij.lsp

import com.intellij.openapi.components.*

@Service(Service.Level.APP)
@State(
    name = "RoxyLspSettings",
    storages = [Storage("RoxyLspSettings.xml")]
)
class RoxyLspSettings : PersistentStateComponent<RoxyLspSettings.State> {
    data class State(
        var lspServerPath: String = ""
    )

    private var state = State()

    override fun getState(): State = state

    override fun loadState(state: State) {
        this.state = state
    }

    var lspServerPath: String
        get() = state.lspServerPath
        set(value) { state.lspServerPath = value }

    companion object {
        fun getInstance(): RoxyLspSettings = service()
    }
}
