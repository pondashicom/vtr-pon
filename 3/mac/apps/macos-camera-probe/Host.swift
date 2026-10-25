import AppKit
import SystemExtensions

final class ProbeHost: NSObject, NSApplicationDelegate, OSSystemExtensionRequestDelegate {
    let identifier = "com.pondashicom.vtrpon.CameraProbe.Extension"
    var window: NSWindow!
    let status = NSTextField(wrappingLabelWithString: "未インストールの場合は「有効化」を押してください。")
    private var pending: OSSystemExtensionRequest?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        let menu = NSMenu()
        let item = NSMenuItem(); menu.addItem(item)
        item.submenu = NSMenu(); item.submenu?.addItem(withTitle: "終了", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        NSApp.mainMenu = menu
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 620, height: 310), styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
        window.title = "VTR-PON Camera Test"
        let title = NSTextField(labelWithString: "仮想カメラの小規模試作")
        title.font = .boldSystemFont(ofSize: 22)
        let description = NSTextField(wrappingLabelWithString: "1920×1080・30 fpsのカラーバー、動く白い四角、フレーム番号を送ります。\nカメラ一覧では「VTR-PON Camera Test」を選んでください。")
        let install = NSButton(title: "有効化", target: self, action: #selector(activate))
        let remove = NSButton(title: "削除", target: self, action: #selector(deactivate))
        let settings = NSButton(title: "拡張の設定を開く", target: self, action: #selector(openSettings))
        let buttons = NSStackView(views: [install, remove, settings]); buttons.spacing = 12
        let stack = NSStackView(views: [title, description, buttons, status]); stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 22
        stack.translatesAutoresizingMaskIntoConstraints = false
        window.contentView!.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: window.contentView!.leadingAnchor, constant: 26),
            stack.trailingAnchor.constraint(equalTo: window.contentView!.trailingAnchor, constant: -26),
            stack.topAnchor.constraint(equalTo: window.contentView!.topAnchor, constant: 26)
        ])
        window.center(); window.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
        if CommandLine.arguments.contains("--activate") { activate() }
        if CommandLine.arguments.contains("--deactivate") { deactivate() }
    }
    func report(_ text: String) { status.stringValue = text; print(text); fflush(stdout) }
    func submit(_ activation: Bool) {
        guard pending == nil else { report("前の要求の完了を待っています。"); return }
        guard Bundle.main.bundleURL.path.hasPrefix("/Applications/") else {
            report("この試作アプリを /Applications に配置してから実行してください。"); return
        }
        let request = activation ? OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: identifier, queue: .main) : OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: identifier, queue: .main)
        request.delegate = self; pending = request
        report(activation ? "macOSへ有効化を要求しています…" : "macOSへ削除を要求しています…")
        OSSystemExtensionManager.shared.submitRequest(request)
    }
    @objc func activate() { submit(true) }
    @objc func deactivate() { submit(false) }
    @objc func openSettings() {
        NSWorkspace.shared.open(URL(string: "x-apple.systempreferences:com.apple.LoginItems-Settings.extension")!)
    }
    func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
        report("macOSの許可待ちです。「一般 → ログイン項目と機能拡張 → カメラ機能拡張」で有効にしてください。")
    }
    func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties, withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction { .replace }
    func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
        pending = nil
        report(result == .completed ? "要求が完了しました。受信アプリでカメラを選択してください。" : "要求を受け付けました。完了にはMacの再起動が必要です。")
    }
    func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
        pending = nil; report("要求に失敗しました: \(error as NSError)")
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

@main enum HostMain {
    static func main() {
        let delegate = ProbeHost()
        NSApplication.shared.delegate = delegate
        withExtendedLifetime(delegate) { NSApplication.shared.run() }
    }
}
