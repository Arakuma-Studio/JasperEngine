// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
// Node >=20, animejs@4.5.0 and jsdom@26.1.0 installed outside the repository:
// node gen_text_reference.mjs /path/to/node_modules output.json
// Layout is tested in-engine; jsdom supplies the DOM for upstream split/seek semantics.
import { writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

const modules = process.argv[2];
if (!modules || !process.argv[3]) throw new Error('usage: gen_text_reference.mjs NODE_MODULES OUTPUT');
const { JSDOM } = await import(pathToFileURL(join(modules, 'jsdom/lib/api.js')));
const { window } = new JSDOM('<!DOCTYPE html><body></body>');
Object.assign(globalThis, {
  window, document: window.document, HTMLElement: window.HTMLElement,
  SVGElement: window.SVGElement, NodeList: window.NodeList,
  HTMLCollection: window.HTMLCollection, Node: window.Node,
  getComputedStyle: window.getComputedStyle.bind(window),
  requestAnimationFrame: () => 0, cancelAnimationFrame: () => {},
  ResizeObserver: class { observe() {} disconnect() {} },
});
window.requestAnimationFrame = globalThis.requestAnimationFrame;
window.cancelAnimationFrame = globalThis.cancelAnimationFrame;
document.fonts = { status: 'loaded', ready: Promise.resolve() };
const anime = await import(pathToFileURL(join(modules, 'animejs/dist/modules/index.js')));
anime.engine.useDefaultMainLoop = false;
const samples = [
  { html: 'Hola <em>niño</em> &amp; adiós', params: { chars: true, accessible: false } },
  { html: 'á ñ e\u0301 👩‍👩‍👧‍👦 🇪🇸', params: { chars: true, accessible: false } },
  { html: 'A B', params: { chars: true, includeSpaces: true, accessible: false } },
];
const reference = { version: 'animejs 4.5.0', dom: 'jsdom 26.1.0', samples: [] };
for (const sample of samples) {
  const target = document.createElement('p');
  target.innerHTML = sample.html;
  document.body.appendChild(target);
  const split = anime.splitText(target, sample.params);
  reference.samples.push({ ...sample,
    words: split.words.map(el => el.textContent), chars: split.chars.map(el => el.textContent) });
  split.revert();
  target.remove();
}
const target = document.createElement('p');
target.textContent = 'ñáe\u0301';
document.body.appendChild(target);
const split = anime.splitText(target, { chars: true, accessible: false });
const animation = anime.animate(split.chars, {
  translateY: [100, 0], duration: 500, delay: anime.stagger(100), ease: 'linear', autoplay: false,
});
reference.seek = [0, 250, 500, 750].map(time => {
  animation.seek(time);
  return { time, translateY: split.chars.map(el => parseFloat(el.style.transform.match(/translateY\(([^)]+)/)[1])) };
});
animation.revert();
split.revert();
writeFileSync(process.argv[3], JSON.stringify(reference, null, 2) + '\n');
