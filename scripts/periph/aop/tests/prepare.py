#!/usr/bin/env python3
"""Offline payload packaging: preserve the baseline, opt in only explicitly."""
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
import runpy
import tempfile
from unittest.mock import patch


def main():
    prepare = runpy.run_path(str(Path(__file__).resolve().parents[1] / 'prepare-tests.py'))['main']
    with tempfile.TemporaryDirectory(prefix='aop-payload-') as temp:
        root = Path(temp)
        prepare.__globals__['ROOT'] = root
        kernel = root / 'build/kernel-aopint'
        kernel.mkdir(parents=True)
        (kernel / 'DIRTY').write_text('')
        (kernel / 'COMMIT').write_text('a' * 40 + ' test kernel\n')
        keys = ('APPLE_A9_AOP', 'APPLE_A9_AOP_IIO', 'APPLE_A9_AOP_ACTIVE',
                'DEBUG_FS', 'KEXEC_FILE', 'NVME_PCI_WRITE_WINDOW')
        (kernel / 'config').write_text(''.join(f'CONFIG_{key}=y\n' for key in keys))
        baseline = root / 'baseline.cmdline'
        original = ['root=/dev/nbd0', 'console=tty0', 'apple_pmgr.kexec_keep_on=1',
                    'panic=0', 'apple_wdt.cold_restart=0', 'unrelated=value']
        baseline.write_text(' '.join(original + ['linux_a9.entry=old',
                            'apple_a9_aop.attach=1', 'apple_a9_aop.active=1']) + '\n')
        common = ['prepare-tests.py', '--tree', 'aopint', '--base-cmdline', str(baseline)]
        with patch('sys.argv', common + ['--attach']), patch('subprocess.run') as run, redirect_stdout(StringIO()):
            prepare()
        assert run.call_count == 3
        lines = [call.args[0][2:] for call in run.call_args_list]
        assert all(line[:len(original)] == original for line in lines)
        for line, active, attach in zip(lines, (0, 1, 1), (0, 0, 1)):
            for key, value in (('active', active), ('attach', attach)):
                assert [arg for arg in line if arg.startswith(f'apple_a9_aop.{key}=')] == [f'apple_a9_aop.{key}={value}']
        r4 = root / 'build/periph/aop-r4'
        assert 'apple_a9_aop.attach=1' in (r4 / 'attach.cmdline').read_text()
        assert f'AOP_ATTACH=aop-r4-attach-{"a" * 10}\n' in (r4 / 'entries.env').read_text()
        with patch('sys.argv', common + ['--attach', '--round', '5']), patch('subprocess.run') as run, redirect_stdout(StringIO()):
            prepare()
        r5 = root / 'build/periph/aop-r5'
        assert run.call_count == 3
        assert run.call_args.kwargs['env']['PAYLOAD_OUT'] == str(r5 / 'm1n1-aop-attach.bin')
        assert f'linux_a9.entry=aop-r5-attach-{"a" * 10}' in run.call_args.args[0]
        assert 'apple_a9_aop.active=1 apple_a9_aop.attach=1' in (r5 / 'attach.cmdline').read_text()
        assert f'AOP_ATTACH=aop-r5-attach-{"a" * 10}\n' in (r5 / 'entries.env').read_text()
        with patch('sys.argv', common + ['--attach', '--round', '6']), patch('subprocess.run') as run, redirect_stdout(StringIO()):
            prepare()
        r6 = root / 'build/periph/aop-r6'
        assert run.call_count == 3
        assert run.call_args.kwargs['env']['PAYLOAD_OUT'] == str(r6 / 'm1n1-aop-attach.bin')
        assert f'linux_a9.entry=aop-r6-attach-{"a" * 10}' in run.call_args.args[0]
        assert 'apple_a9_aop.active=1 apple_a9_aop.attach=1' in (r6 / 'attach.cmdline').read_text()
        with patch('sys.argv', common + ['--attach', '--round', '7']), patch('subprocess.run') as run, redirect_stdout(StringIO()):
            prepare()
        r7 = root / 'build/periph/aop-r7'
        assert run.call_count == 3
        assert run.call_args.kwargs['env']['PAYLOAD_OUT'] == str(r7 / 'm1n1-aop-attach.bin')
        assert f'linux_a9.entry=aop-r7-attach-{"a" * 10}' in run.call_args.args[0]
        assert 'apple_a9_aop.active=1 apple_a9_aop.attach=1' in (r7 / 'attach.cmdline').read_text()
        with patch('sys.argv', common), patch('subprocess.run') as run, redirect_stdout(StringIO()):
            prepare()
        assert run.call_count == 1
        assert 'apple_a9_aop.active=0' in run.call_args.args[0]
        (kernel / 'DIRTY').write_text(' M drivers/soc/apple/apple-a9-aop-active.c\n')
        with patch('sys.argv', common + ['--attach']), patch('subprocess.run') as run:
            try:
                prepare()
            except SystemExit as exc:
                assert 'dirty integration build' in str(exc)
            else:
                raise AssertionError('accepted a dirty integration build')
        run.assert_not_called()
    print('PASS: --tree aopint --attach Round 4/5/6/7 packaging, independent active/attach gates, baseline protections, dirty-build refusal (mkpayload mocked)')


if __name__ == '__main__':
    main()
