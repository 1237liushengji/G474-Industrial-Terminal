# -*- coding: utf-8 -*-
"""A-01: single config point check (docs/04 §1, docs/05 acceptance matrix).

Board facts (peripheral instance names, pin numbers, AF numbers, the bus
address the register contract was frozen around) must appear ONLY in BSP/
(in practice: App/app_board_cfg.h + the BSP drivers).  Everything else must
go through the macros.

Token classes checked in App/ Common/ Core/ Gateway/ Network/ Protocol/ Storage:
  - USART1..3 instance names
  - GPIO_PIN_<n>
  - GPIO_AF<n>
  - 0x500   (register-contract CAN base id literal)
  - 115200  (bare baud literal; allowed in Protocol/ where the Modbus baud
             TABLE is contract data - docs/03 freezes 9600..115200)

Comments are stripped before matching; identifier fragments like
MB_BAUD_IDX_115200 do not count as a "bare" literal.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCAN_DIRS = ['App', 'Common', 'Core', 'Gateway', 'Network', 'Protocol', 'Storage']
EXTS = ('.c', '.h')

RE_BLOCK = re.compile(r'/\*.*?\*/', re.S)
RE_LINE = re.compile(r'//[^\n]*')

CHECKS = [
    ('USART instance', re.compile(r'\bUSART[123]\b'), None),
    ('GPIO_PIN_n', re.compile(r'\bGPIO_PIN_[0-9]+\b'), None),
    ('GPIO_AFn', re.compile(r'\bGPIO_AF[0-9]+\b'), None),
    ('register base id', re.compile(r'\b0x500\b'), None),
    ('bare baud', re.compile(r'(?<![A-Za-z0-9_])115200(?![0-9])'), 'Protocol'),
]


def strip_comments(text):
    text = RE_BLOCK.sub(' ', text)
    text = RE_LINE.sub(' ', text)
    return text


def main():
    findings = []
    total = 0

    for d in SCAN_DIRS:
        base = ROOT / d
        if not base.is_dir():
            continue
        for f in sorted(base.rglob('*')):
            if f.suffix.lower() not in EXTS:
                continue
            text = strip_comments(f.read_text(encoding='utf-8', errors='replace'))
            for line_no, line in enumerate(text.splitlines(), 1):
                for name, pat, allow_dir in CHECKS:
                    if pat.search(line):
                        rel = f.relative_to(ROOT)
                        if allow_dir and str(rel).replace('\\', '/').startswith(allow_dir + '/'):
                            continue
                        findings.append('%s:%d  [%s]  %s' % (rel, line_no, name, line.strip()))
                        total += 1

    if findings:
        print('A-01 FAIL: %d bare board literal(s) outside BSP/:' % total)
        for x in findings:
            print(' ', x)
        return 1

    print('A-01 PASS: 0 bare board literals outside BSP/ '
          '(USART instance / GPIO_PIN / GPIO_AF / 0x500 / 115200)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
