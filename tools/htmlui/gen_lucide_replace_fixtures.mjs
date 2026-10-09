// Writes engine/tests/htmlui/fixtures/compat/runtime/lucide-replace-element.json, the expected
// <svg> attributes of compat_lucide_test.cpp (LucideIcons::svgAttributes):
//
//   node tools/htmlui/gen_lucide_replace_fixtures.mjs > engine/tests/htmlui/fixtures/compat/runtime/lucide-replace-element.json
//
// Transcription of Lucide 1.53.0 packages/lucide/src/replaceElement.ts (+ @lucide/shared
// hasA11yProp, mergeClasses, getClassNames) producing the root <svg> attributes.
const defaultAttributes = { xmlns: 'http://www.w3.org/2000/svg', width: 24, height: 24, viewBox: '0 0 24 24',
  fill: 'none', stroke: 'currentColor', 'stroke-width': 2, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' };
const hasA11yProp = (props) => { for (const prop in props) { if (prop.startsWith('aria-') || prop === 'role' || prop === 'title') return true; } return false; };
const mergeClasses = (...classes) => classes.filter((c, i, a) => Boolean(c) && c.trim() !== '' && a.indexOf(c) === i).join(' ').trim();
const getClassNames = (attrs) => { if (typeof attrs === 'string') return attrs; if (!attrs || !attrs.class) return ''; if (attrs.class && typeof attrs.class === 'string') return attrs.class.split(' '); if (Array.isArray(attrs.class)) return attrs.class; return ''; };
function replace(elementAttrList) {
  const elementAttrs = {}; for (const [k, v] of elementAttrList) elementAttrs[k] = v;
  const iconName = elementAttrs['data-lucide'];
  const ariaProps = hasA11yProp(elementAttrs) ? {} : { 'aria-hidden': 'true' };
  const attrs = {};
  const iconAttrs = { ...defaultAttributes, 'data-lucide': iconName, ...ariaProps, ...attrs, ...elementAttrs };
  const classNames = mergeClasses('lucide', `lucide-${iconName}`, ...getClassNames(elementAttrs), ...getClassNames(attrs));
  if (classNames) Object.assign(iconAttrs, { class: classNames });
  // createElement: { ...defaultAttributes, ...customAttrs }, setAttribute in key order
  const finalAttrs = { ...defaultAttributes, ...iconAttrs };
  return Object.keys(finalAttrs).map((k) => [k, String(finalAttrs[k])]);
}
const cases = [
  [['data-lucide', 'house']],
  [['data-lucide', 'home'], ['class', 'icon big']],
  [['data-lucide', 'ArrowRight'], ['class', 'a  b a']],
  [['data-lucide', 'arrowRight'], ['aria-label', 'Next']],
  [['data-lucide', 'heart'], ['role', 'img']],
  [['data-lucide', 'star'], ['title', 'Favourite']],
  [['data-lucide', 'settings'], ['width', '32'], ['height', '32'], ['stroke-width', '1.5']],
  [['id', 'menu-icon'], ['data-lucide', 'menu'], ['style', 'color: red']],
  [['data-lucide', 'x'], ['class', 'lucide foo lucide-x']],
  [['class', ''], ['data-lucide', 'check']],
  [['class', 'c'], ['data-lucide', 'circle'], ['fill', 'currentColor'], ['aria-hidden', 'false']],
];
console.log(JSON.stringify({
  source: 'Lucide 1.53.0 packages/lucide/src/replaceElement.ts, transcribed (gen_replace.mjs, see compat_lucide_test.cpp)',
  cases: cases.map((c) => ({ element: c, svg: replace(c) })) }, null, 1));
