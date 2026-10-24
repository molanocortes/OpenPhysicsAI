// html2pdf.swift - an HTML file to an A4 PDF with the system's own WebKit, no dependency.
//   swiftc -O tools/html2pdf.swift -o build/html2pdf && build/html2pdf in.html out.pdf
// The page's own CSS sets the margins (@page is not honoured by WebKit's print path, so the body carries them).
import Cocoa
import WebKit

final class Job: NSObject, WKNavigationDelegate {
    let out: URL
    let web: WKWebView
    let window: NSWindow
    init(src: URL, out: URL) {
        self.out = out
        let frame = NSRect(x: 0, y: 0, width: 794, height: 1123) // A4 at 96 dpi
        web = WKWebView(frame: frame)
        window = NSWindow(contentRect: frame, styleMask: [.borderless], backing: .buffered, defer: false)
        super.init()
        window.contentView = web
        web.navigationDelegate = self
        web.loadFileURL(src, allowingReadAccessTo: src.deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent())
    }
    func webView(_ w: WKWebView, didFinish n: WKNavigation!) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
            let info = NSPrintInfo()
            info.paperSize = NSSize(width: 595.28, height: 841.89)
            info.topMargin = 0; info.bottomMargin = 0; info.leftMargin = 0; info.rightMargin = 0
            info.horizontalPagination = .fit
            info.verticalPagination = .automatic
            info.jobDisposition = .save
            info.dictionary()[NSPrintInfo.AttributeKey.jobSavingURL] = self.out
            let op = w.printOperation(with: info)
            op.showsPrintPanel = false
            op.showsProgressPanel = false
            op.view?.frame = NSRect(x: 0, y: 0, width: 595.28, height: 841.89)
            op.runModal(for: self.window, delegate: self, didRun: #selector(self.done(_:success:contextInfo:)), contextInfo: nil)
        }
    }
    @objc func done(_ op: NSPrintOperation, success: Bool, contextInfo: UnsafeMutableRawPointer?) {
        FileHandle.standardError.write((success ? "written \(out.path)\n" : "printing failed\n").data(using: .utf8)!)
        exit(success ? 0 : 1)
    }
    func webView(_ w: WKWebView, didFail n: WKNavigation!, withError e: Error) { print("load failed: \(e)"); exit(2) }
}

let a = CommandLine.arguments
guard a.count == 3 else { print("usage: html2pdf in.html out.pdf"); exit(64) }
let app = NSApplication.shared
app.setActivationPolicy(.prohibited)
let job = Job(src: URL(fileURLWithPath: a[1]), out: URL(fileURLWithPath: a[2]))
_ = job
app.run()
