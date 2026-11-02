// The page's half of the crosscheck: fetch the cases and the clips from
// crosscheck.py's server, render each case through
// window.ramdacDemo.renderCase, and post the picture back, then the names of
// any uniform the page set that a program does not have. crosscheck.py
// compares; nothing is judged here.
const post = (path, body) => fetch(path, { method: 'POST', body });

async function main() {
  for (let i = 0; i < 200 && !window.ramdacDemo; i += 1) await new Promise((r) => setTimeout(r, 50));
  if (!window.ramdacDemo) throw new Error('window.ramdacDemo never appeared: the page did not start');
  const job = await (await fetch('/xc/job.json')).json();
  const clips = {};
  for (const c of job.cases) {
    if (!clips[c.clip]) clips[c.clip] = new Uint8Array(await (await fetch(`/xc/clip/${c.clip}`)).arrayBuffer());
    const out = window.ramdacDemo.renderCase({
      settings: c.settings, frames: clips[c.clip], width: job.width, height: job.height, fps: job.fps, count: c.count,
    });
    await post(`/xc/out/${c.name}`, out);
  }
  await post('/xc/missing', JSON.stringify(window.ramdacDemo.missing()));
  await post('/xc/done', 'ok');
}

main().catch((error) => post('/xc/error', String(error?.stack ?? error)));
