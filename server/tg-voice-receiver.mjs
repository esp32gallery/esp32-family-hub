import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { execFile } from 'node:child_process';
import crypto from 'node:crypto';
import { promisify } from 'node:util';
const run = promisify(execFile);
const home = os.homedir();
const tokenFile = path.join(home, '.config/voice-inbox/telegram-voice-token');
const token = fs.readFileSync(tokenFile, 'utf8').trim();
if (!token) throw new Error('telegram-voice-token missing');
const chatId = String(process.env.VOICE_TG_CHAT_ID || 795477335);
const inbox = process.env.VOICE_INBOX_URL || 'http://127.0.0.1:8896';
const stateFile = path.join(home, '.config/voice-inbox', 'tg-voice-offset-' + crypto.createHash('sha256').update(token).digest('hex').slice(0,12));
const tmpDir = path.join(os.tmpdir(), 'family-tg-voice');
fs.mkdirSync(tmpDir, { recursive: true });
let offset = Number(fs.existsSync(stateFile) ? fs.readFileSync(stateFile, 'utf8').trim() : 0) || 0;
const log = (...a) => console.log(new Date().toISOString(), ...a);
async function api(method, query = {}) {
  const qs = new URLSearchParams(query).toString();
  const url = `https://api.telegram.org/bot${token}/${method}${qs ? '?' + qs : ''}`;
  const { stdout } = await run('curl', ['-sS', '-x', 'http://127.0.0.1:7890', '-m', '35', url], { maxBuffer: 4 * 1024 * 1024 });
  const j = JSON.parse(stdout);
  if (!j.ok) throw new Error(`${method}: ${JSON.stringify(j).slice(0, 200)}`);
  return j.result;
}
async function handle(update) {
  const m = update.message;
  if (!m || String(m.chat?.id) !== chatId) return;
  const media = m.voice || m.audio;
  if (!media) {
    if (m.text === '/start') await api('sendMessage', {
      chat_id: chatId, text: '已连接家庭语音。宝宝的语音会发到这里，直接发语音就能回复小板子。',
    });
    return;
  }
  const info = await api('getFile', { file_id: media.file_id });
  const src = path.join(tmpDir, `tg-${update.update_id}`);
  const out = src + '.wav';
  const url = `https://api.telegram.org/file/bot${token}/${info.file_path}`;
  await run('curl', ['-sS', '-x', 'http://127.0.0.1:7890', '-L', '-m', '120', '-o', src, url]);
  const ffmpeg = '/opt/homebrew/bin/ffmpeg';
  await run(ffmpeg, ['-nostdin', '-hide_banner', '-loglevel', 'error', '-i', src,
    '-ac', '1', '-ar', '32000', '-c:a', 'pcm_s16le', '-f', 'wav', out], { timeout: 30000 });
  const body = fs.readFileSync(out);
  const res = await fetch(`${inbox}/voice?from=dad&source=${path.basename(stateFile)}-${update.update_id}`, {
    method: 'POST', headers: { 'content-type': 'audio/wav' }, body,
  });
  if (!res.ok) throw new Error(`voice-inbox ${res.status}`);
  const saved = await res.json();
  log('Telegram voice forwarded', saved.id);
  await api('sendMessage', { chat_id: chatId, text: '✓ 已送到家庭语音', reply_to_message_id: String(m.message_id) }).catch(() => log('receipt failed'));
  fs.rmSync(src, { force: true }); fs.rmSync(out, { force: true });
}
log('receiver started', { offset });
for (;;) {
  try {
    const updates = await api('getUpdates', {
      offset: String(offset), timeout: '20', allowed_updates: JSON.stringify(['message']),
    });
    for (const update of updates) {
      await handle(update);
      offset = Math.max(offset, update.update_id + 1);
      fs.writeFileSync(stateFile + '.tmp', String(offset), { mode: 0o600 });
      fs.renameSync(stateFile + '.tmp', stateFile);
    }
  } catch (e) {
    log('poll/forward failed', String(e.message).replace(/bot[0-9]+:[A-Za-z0-9_-]+/g, 'bot[redacted]'));
    await new Promise(r => setTimeout(r, 4000));
  }
}
