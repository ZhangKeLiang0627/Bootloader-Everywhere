// 网页端载体层单测。与 tools/test_protocol.cpp（C）和 TestApp/tools/proto.py（Python）
// 共用同一组向量 —— 三处都过，才算「三份实现的 CRC 变体与字节序一致」。
//
// 运行：node tools/test_protocol_js.mjs

import { selftest } from '../docs/js/protocol.js';

const { cases, fails } = selftest();

console.log('=== 网页端载体层自检 ===');
if (fails.length) {
  for (const f of fails) console.log('  ✘ ' + f);
  console.log(`\n${cases} 项断言，${fails.length} 项失败 → FAIL`);
  process.exitCode = 1;
} else {
  console.log(`  ${cases} 项断言全部通过 → PASS`);
}
