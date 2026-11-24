// Epic Games sign-in for builds made with --epic account (games the Epic launcher installed, with --eos real): the
// player signs in on Epic's own website, with any of the ways Epic offers there, and pastes back the code Epic shows.
// The runtime (hle_epic.c) turns the code into the launcher's one-time exchange code over TLS it does itself, through
// WISP when the build has it, and passes it to the game as the launcher does. Passwords never reach the page.
//
// Only Epic's refresh token is kept, in this browser's storage, when "Remember me" is ticked, so later launches sign
// in at once (?epiclogout=1 forgets it, ?epic=off skips the sign-in). Nothing reaches UEWASM or the host.
addToLibrary({
  $uewEpic__deps: ['$UTF8ToString', '$stringToUTF8'],
  $uewEpic: {
    s: { state: 0, cred: '', keep: true },
    CLIENT: '34a02cf8f4414e29b15921876da36f9a',
    KEY: 'uew_epic_session',
    loginUrl() {
      const back = 'https://www.epicgames.com/id/api/redirect?clientId=' + this.CLIENT + '&responseType=code';
      return 'https://www.epicgames.com/id/login?redirectUrl=' + encodeURIComponent(back);
    },
    log(t) { console.log('[epic] ' + t); },
    // the code from what the player pasted: Epic's answer (JSON), a link with ?code=, or the code alone
    code(text) {
      const t = text.trim();
      const m = t.match(/"authorizationCode"\s*:\s*"([0-9a-f]+)"/i) || t.match(/[?&]code=([0-9a-f]+)/i) || t.match(/^([0-9a-f]{32})$/i);
      return m ? m[1] : null;
    },
    dialog(msg) {
      const box = document.createElement('div');
      const field = 'width:100%;box-sizing:border-box;margin:6px 0;padding:11px 12px;background:#121212;border:1px solid #3a3a3a;color:#fff;border-radius:4px;font:inherit';
      box.style.cssText = 'position:fixed;inset:0;z-index:2147483647;display:flex;align-items:center;justify-content:center;background:#000c;font:14px system-ui,sans-serif;color:#fff';
      box.innerHTML = `<form style="width:400px;max-width:calc(100vw - 32px);box-sizing:border-box;background:#202020;padding:28px 28px 22px;border-radius:8px;box-shadow:0 8px 32px #000a">
        <div style="font-size:20px;font-weight:700;margin-bottom:6px">Sign in with Epic Games</div>
        <div style="font-size:13px;color:#aaa;margin-bottom:18px;line-height:1.45">This game uses your Epic Games account. You sign in on Epic's own website, so your password never comes here.</div>
        <div style="font-size:13px;font-weight:600;margin-bottom:6px">1. Sign in on epicgames.com</div>
        <button type="button" data-open style="width:100%;padding:12px;border:0;border-radius:4px;background:#26bbff;color:#101014;font-weight:700;cursor:pointer;font:inherit;font-weight:700">Open Epic Games sign-in</button>
        <div style="font-size:12px;color:#aaa;margin:8px 0 18px;line-height:1.45">Every sign-in option Epic has is there: email, Google, Apple, Xbox, PlayStation, Nintendo, Steam, Facebook and LEGO.
          <a href="#" data-other style="color:#26bbff">Use a different account</a></div>
        <div style="font-size:13px;font-weight:600;margin-bottom:2px">2. Paste what Epic shows</div>
        <div style="font-size:12px;color:#aaa;line-height:1.45">After you sign in, Epic opens a short page of text. Copy all of it (or the authorizationCode) and paste it here.</div>
        <input name="c" placeholder="Paste here" autocomplete="off" spellcheck="false" style="${field}">
        <label style="display:block;margin:8px 0;font-size:12px;color:#ddd"><input name="r" type="checkbox" ${this.s.keep ? 'checked' : ''}> Remember me in this browser</label>
        <div data-msg style="min-height:18px;font-size:12px;color:#ff8f8f;margin:6px 0"></div>
        <button style="width:100%;padding:12px;border:0;border-radius:4px;background:#26bbff;color:#101014;cursor:pointer;font:inherit;font-weight:700">Continue</button>
        <div style="text-align:right;margin-top:12px"><a href="#" data-skip style="color:#aaa;font-size:12px">Play without Epic</a></div>
      </form>`;
      document.body.appendChild(box);
      const f = box.querySelector('form'), say = t => { box.querySelector('[data-msg]').textContent = t; };
      if (msg) say(msg);
      box.querySelector('[data-open]').onclick = () => window.open(this.loginUrl(), '_blank', 'noopener');
      box.querySelector('[data-other]').onclick = e => {
        e.preventDefault();
        window.open('https://www.epicgames.com/id/logout?redirectUrl=' + encodeURIComponent(this.loginUrl()), '_blank', 'noopener');
      };
      box.querySelector('[data-skip]').onclick = e => { e.preventDefault(); box.remove(); this.log('playing without Epic'); this.s.state = -1; };
      f.onsubmit = e => {
        e.preventDefault();
        const c = this.code(f.c.value);
        if (!c) { say("That doesn't contain a sign-in code: copy the whole page Epic showed after you signed in"); return; }
        this.s.keep = f.r.checked;
        this.s.cred = 'authorization_code\n' + c;
        box.remove();
        this.s.state = 1;
      };
    },
    start() {
      const q = new URLSearchParams(location.search), s = this.s;
      if (q.get('epiclogout') === '1') try { localStorage.removeItem(this.KEY); } catch (e) { }
      let saved = null;
      try { saved = JSON.parse(localStorage.getItem(this.KEY) || 'null'); } catch (e) { }
      if (saved && saved.refresh) { s.cred = 'refresh_token\n' + saved.refresh; s.state = 1; this.log('signing in as ' + (saved.name || '?') + ' (remembered)'); }
      else this.dialog();
    },
    done(ok, refresh, name, msg) {
      const s = this.s;
      s.cred = '';
      if (ok) {
        try { if (s.keep && refresh) localStorage.setItem(this.KEY, JSON.stringify({ refresh, name })); else localStorage.removeItem(this.KEY); } catch (e) { }
        return;
      }
      try { localStorage.removeItem(this.KEY); } catch (e) { }
      this.log('sign-in failed: ' + msg);
      s.state = 0;
      this.dialog(msg + ' (sign in again)');
    },
  },

  // 1 when this build signs in to an Epic account; starts the sign-in (remembered token, else the dialog)
  epic_start__proxy: 'sync', epic_start__deps: ['$uewEpic'], epic_start__sig: 'i',
  epic_start: function () {
    if (typeof location === 'undefined' || typeof document === 'undefined') return 0;
    const cfg = Module.uewEpic || {};
    const mode = new URLSearchParams(location.search).get('epic') || cfg.mode;
    if (mode !== 'account') return 0;
    Module.uewEpicClient = uewEpic;
    uewEpic.start();
    return 1;
  },
  // 0 waiting for the player, 1 a code or token to use, -1 playing without Epic
  epic_state__proxy: 'sync', epic_state__deps: ['$uewEpic'], epic_state__sig: 'i',
  epic_state: function () { return uewEpic.s.state; },
  // "grant\nvalue": authorization_code or refresh_token
  epic_cred__proxy: 'sync', epic_cred__deps: ['$uewEpic'], epic_cred__sig: 'ipi',
  epic_cred: function (buf, cap) { stringToUTF8(uewEpic.s.cred, buf, cap); return 1; },
  epic_namespace__proxy: 'sync', epic_namespace__deps: ['$uewEpic'], epic_namespace__sig: 'ipi',
  epic_namespace: function (buf, cap) { stringToUTF8((Module.uewEpic || {}).namespace || '', buf, cap); return 1; },
  epic_done__proxy: 'sync', epic_done__deps: ['$uewEpic'], epic_done__sig: 'vippp',
  epic_done: function (ok, refresh, name, msg) {
    uewEpic.done(ok, refresh ? UTF8ToString(refresh) : '', name ? UTF8ToString(name) : '', msg ? UTF8ToString(msg) : '');
  },
});
