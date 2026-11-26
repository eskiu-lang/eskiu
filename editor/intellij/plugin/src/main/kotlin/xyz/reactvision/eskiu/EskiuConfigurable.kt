package xyz.reactvision.eskiu

import com.intellij.openapi.options.Configurable
import com.intellij.ui.components.JBCheckBox
import com.intellij.ui.components.JBTextField
import com.intellij.util.ui.FormBuilder
import javax.swing.JComponent
import javax.swing.JPanel

class EskiuConfigurable : Configurable {
    private val compiler = JBTextField()
    private val node = JBTextField()
    private val flags = JBTextField()
    private val entry = JBTextField()
    private val onType = JBCheckBox("Check while typing, not only when a file is opened or saved")
    private val delay = JBTextField()

    override fun getDisplayName(): String = "Eskiu"

    override fun createComponent(): JComponent {
        compiler.emptyText.text = "build22/eskiuc or build/eskiuc in the project, ~/.eskiu/bin/eskiuc, else PATH"
        node.emptyText.text = "node on PATH, Homebrew, fnm, nvm or Volta"
        flags.emptyText.text = "e.g. -Wall -Wno-format (passed to every check)"
        entry.emptyText.text = "e.g. src/main.esk (empty: eskiu.toml's main, else the file's importers)"
        delay.emptyText.text = "400"
        val panel: JPanel = FormBuilder.createFormBuilder()
            .addLabeledComponent("eskiuc path:", compiler)
            .addLabeledComponent("Node.js path:", node)
            .addLabeledComponent("Compiler flags:", flags)
            .addLabeledComponent("Entry point:", entry)
            .addComponent(onType)
            .addLabeledComponent("Check delay (ms):", delay)
            .addComponentFillVertically(JPanel(), 0)
            .panel
        reset()
        return panel
    }

    private fun delayValue(): Int = delay.text.trim().toIntOrNull()?.coerceAtLeast(100) ?: 400

    override fun isModified(): Boolean {
        val s = EskiuSettings.get().state
        return compiler.text.trim() != s.compilerPath || node.text.trim() != s.nodePath ||
            flags.text.trim() != s.compilerFlags || entry.text.trim() != s.entryPoint ||
            onType.isSelected != s.checkOnType || delayValue() != s.checkDelay
    }

    override fun apply() {
        val s = EskiuSettings.get().state
        s.compilerPath = compiler.text.trim()
        s.nodePath = node.text.trim()
        s.compilerFlags = flags.text.trim()
        s.entryPoint = entry.text.trim()
        s.checkOnType = onType.isSelected
        s.checkDelay = delayValue()
    }

    override fun reset() {
        val s = EskiuSettings.get().state
        compiler.text = s.compilerPath
        node.text = s.nodePath
        flags.text = s.compilerFlags
        entry.text = s.entryPoint
        onType.isSelected = s.checkOnType
        delay.text = s.checkDelay.toString()
    }
}
