package xyz.reactvision.eskiu

import com.intellij.execution.configurations.GeneralCommandLine
import com.intellij.execution.configurations.PathEnvironmentVariableUtil
import com.intellij.ide.impl.isTrusted
import com.intellij.openapi.application.PathManager
import com.intellij.openapi.project.Project
import com.redhat.devtools.lsp4ij.LanguageServerFactory
import com.redhat.devtools.lsp4ij.server.OSProcessStreamConnectionProvider
import com.redhat.devtools.lsp4ij.server.StreamConnectionProvider
import java.io.File

/**
 * The Eskiu language server for LSP4IJ: the plugin bundles `editor/vscode/server.js`
 * (the same server Neovim or Helix use), which runs the compiler's editor modes. It
 * gives diagnostics, completion, hover, signature help, go to definition, the
 * structure view and formatting.
 */
class EskiuLanguageServerFactory : LanguageServerFactory {
    override fun createConnectionProvider(project: Project): StreamConnectionProvider = EskiuServer(project)
}

class EskiuServer(project: Project) : OSProcessStreamConnectionProvider() {
    init {
        // The plugin directory holds lib/<our jar> and server/server.js.
        val jar = PathManager.getJarPathForClass(EskiuServer::class.java)
        val server = jar?.let { File(it).parentFile?.parentFile?.resolve("server/server.js")?.path } ?: "server.js"
        val cmd = GeneralCommandLine(findNode(), server)
        project.basePath?.let { cmd.withWorkDirectory(it) }
        findCompiler(project)?.let { cmd.withEnvironment("ESKIUC", it) }
        // only a trusted project's own build directories may provide the compiler
        if (project.isTrusted()) cmd.withEnvironment("ESKIU_SEARCH_WORKSPACE", "1")
        EskiuSettings.get().state.compilerFlags.trim().takeIf { it.isNotEmpty() }?.let { cmd.withEnvironment("ESKIU_FLAGS", it) }
        commandLine = cmd
    }

    private fun executable(path: String): Boolean = File(path).let { it.isFile && it.canExecute() }

    /**
     * The settings' path, else `build22`/`build` in the project when the project is trusted,
     * else `~/.eskiu/bin`, else null (server.js looks on PATH). An untrusted project cannot
     * choose the program the server runs.
     */
    private fun findCompiler(project: Project): String? {
        val configured = EskiuSettings.get().state.compilerPath
        if (configured.isNotEmpty()) return configured
        val base = project.basePath
        if (base != null && project.isTrusted()) {
            for (b in listOf("build22", "build")) {
                val c = "$base/$b/eskiuc"
                if (executable(c)) return c
            }
        }
        val home = System.getProperty("user.home")
        return "$home/.eskiu/bin/eskiuc".takeIf { executable(it) }
    }

    /** An IDE started from the Dock does not get the shell's PATH, so also try the usual installs. */
    private fun findNode(): String {
        val configured = EskiuSettings.get().state.nodePath
        if (configured.isNotEmpty()) return configured
        PathEnvironmentVariableUtil.findInPath("node")?.let { return it.path }
        val home = System.getProperty("user.home")
        val fixed = listOf(
            "/opt/homebrew/bin/node", "/usr/local/bin/node", "/usr/bin/node",
            "$home/.volta/bin/node", "$home/.local/share/fnm/aliases/default/bin/node",
        )
        fixed.firstOrNull { executable(it) }?.let { return it }
        for (root in listOf("$home/.nvm/versions/node", "$home/.local/share/fnm/node-versions")) {
            val versions = File(root).listFiles()?.sortedByDescending { it.name } ?: continue
            for (v in versions) {
                for (p in listOf("bin/node", "installation/bin/node")) {
                    val c = File(v, p)
                    if (c.canExecute()) return c.path
                }
            }
        }
        return "node"
    }
}
