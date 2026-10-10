namespace EFML.Manager.Services;

// Injected into module pages. Derived from Better-Endfield's ThirdPartyWebBridge (AGPL-3.0).
// The window binds the page to its own module and keeps the RPC token out of JavaScript.
// window.efml is also exposed as window.betterEndfield and window.betterEndfieldNext so pages
// written for Better-Endfield work unchanged.
internal static class WebBridge
{
    internal const string Protocol = "efml.module-ui.v1";

    internal static readonly string[] AcceptedProtocols =
        [Protocol, "better-endfield.module-ui.v1", "better-endfield-next.module-ui.v1"];

    internal const string Script = """
    (() => {
      if (window.top !== window || window.efml) return;
      let next = 0; const pending = new Map(), listeners = new Set();
      const session = Math.random().toString(36).slice(2);
      function invoke(operation, payload) {
        const request_id = session + ':' + (++next);
        return new Promise((resolve, reject) => {
          const timeout = setTimeout(() => { pending.delete(request_id); reject(new Error('Host request timed out')); }, 15000);
          pending.set(request_id, { resolve, reject, timeout });
          try {
            window.chrome.webview.postMessage({ protocol: 'efml.module-ui.v1', request_id, operation, payload });
          } catch (error) { clearTimeout(timeout); pending.delete(request_id); reject(error); }
        });
      }
      window.chrome.webview.addEventListener('message', event => {
        const envelope = event.data;
        if (envelope.kind === 'bridge_reply') {
          const item = pending.get(envelope.request_id); if (!item) return;
          clearTimeout(item.timeout); pending.delete(envelope.request_id);
          envelope.error ? item.reject(new Error(envelope.error)) : item.resolve(envelope.value);
        } else if (envelope.kind === 'runtime_message') {
          for (const listener of listeners) { try { listener(envelope.message); } catch (error) { console.error(error); } }
        }
      });
      const bridge = Object.freeze({
        readConfig: () => invoke('readConfig'),
        saveConfig: configuration => invoke('saveConfig', configuration),
        send: body => invoke('send', body),
        status: () => invoke('status'),
        onmessage: listener => {
          if (typeof listener !== 'function') throw new TypeError('Expected a function');
          listeners.add(listener); return () => listeners.delete(listener);
        }
      });
      for (const name of ['efml', 'betterEndfield', 'betterEndfieldNext'])
        Object.defineProperty(window, name, { value: bridge });
    })();
    """;
}
