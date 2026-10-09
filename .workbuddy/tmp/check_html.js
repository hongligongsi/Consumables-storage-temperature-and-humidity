const fs = require('fs');
const path = process.argv[2];
const html = fs.readFileSync(path, 'utf8');
const m = html.match(/<script>([\s\S]*?)<\/script>/);
if (!m) { console.log('NO_SCRIPT'); process.exit(1); }
try { new Function(m[1]); console.log('JS_SYNTAX_OK'); }
catch (e) { console.log('JS_ERR: ' + e.message); process.exit(1); }

const style = html.match(/<style>([\s\S]*?)<\/style>/)[1];
const ob = (style.match(/{/g) || []).length, cb = (style.match(/}/g) || []).length;
console.log('CSS braces ' + ob + '/' + cb + (ob === cb ? ' OK' : ' MISMATCH'));

const src = m[1];
const famCount = (src.match(/fam:\s*'/g) || []).length;
const mCalls = (src.match(/M\('/g) || []).length;
console.log('schemes=' + famCount + '  M() calls=' + mCalls + (famCount * 2 === mCalls ? ' OK(day+night)' : ' CHECK'));

// 校验每个 M() 参数个数与 hex 格式
const argRe = /M\(((?:\s*'[^']*'\s*,?)+)\)/g;
let mm, bad = 0, total = 0;
while ((mm = argRe.exec(src)) !== null) {
  total++;
  const args = mm[1].split(',').map(s => s.trim()).filter(Boolean);
  if (args.length !== 11) { bad++; console.log('  arg count ' + args.length + ' -> ' + mm[1].slice(0, 60)); continue; }
  args.forEach(a => {
    if (!/^'#[0-9A-Fa-f]{6}'$/.test(a)) { bad++; console.log('  bad hex ' + a); }
  });
}
console.log('M() args checked=' + total + (bad ? '  ISSUES=' + bad : '  all 11 hex OK'));

const divO = (html.match(/<div/g) || []).length, divC = (html.match(/<\/div>/g) || []).length;
console.log('static <div> ' + divO + '/' + divC);
