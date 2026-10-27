#!/usr/bin/env python3
"""Exercise the Mac-facing command with a fake SSH transport; no network calls."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE=Path(__file__).resolve().parent
MOCK=r'''#!/usr/bin/env python3
import os,sys,tarfile
from pathlib import Path
p=Path(os.environ['HOST_FIX']); cmd=sys.argv[1]
with (p/'calls').open('a') as f:f.write(cmd+'\n')
old='11111111-1111-1111-1111-111111111111'
if cmd=='cat /proc/sys/kernel/random/boot_id': print(old)
elif 'device-tree/compatible' in cmd: print('apple,j71t')
elif cmd.startswith('mktemp -d'): print('/tmp/linux-a9-entry.MOCK')
elif cmd.startswith('tar -xf'):
 with tarfile.open(fileobj=sys.stdin.buffer,mode='r|') as t:t.extractall(p/'uploaded',filter='data')
elif 'sudo -n bash -s' in cmd: (p/'remote-install').write_bytes(sys.stdin.buffer.read())
elif 'linux-a9-kexec load' in cmd:
 if os.environ.get('FAIL_LOAD')=='1':sys.exit(1)
elif 'systemd-run' in cmd: (p/'reboot-requested').touch()
elif cmd.startswith('printf "boot_id="'):
 count=int((p/'count').read_text()) if (p/'count').exists() else 0
 (p/'count').write_text(str(count+1))
 new='22222222-2222-2222-2222-222222222222'
 print('boot_id='+(old if count==0 or os.environ.get('OLD_ONLY')=='1' else new))
 print('Linux mock 6.19 aarch64')
 print('omarchy.root=nvme linux_a9.entry=host-fixture')
elif 'linux-a9-health-probe' in cmd:
 sys.stdin.read()
 n=int((p/'probes').read_text()) if (p/'probes').exists() else 0
 (p/'probes').write_text(str(n+1))
 before=1000+10*n; after=before if os.environ.get('HEALTH_FIX')=='frozen' else before+4
 irq='  69: {} 0 PCI-MSI-0000:01:00.0 0 Edge nvme0q0, nvme0q1'
 print('boot_id=22222222-2222-2222-2222-222222222222\nuptime=130\nload=0.5')
 print('irq_before='+irq.format(before)+'\nsudo_rc=0\nsudo_ms=5\nread_rc=0\nread_ms=3')
 print('irq_after='+irq.format(after)+'\ndmesg_rc=0\ndone=1')
'''

def image():
 # Any built arm64 kernel will do; bootinteg is no longer kept on disk.
 fixed=HERE.parents[1]/'build/kernel-bootinteg/Image.gz'
 found=sorted(HERE.parents[1].glob('build/kernel-*/Image.gz'),key=lambda x:x.stat().st_mtime)
 if fixed.exists():return fixed
 if not found:raise unittest.SkipTest('no build/kernel-*/Image.gz')
 return found[-1]

class HostTests(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory();self.p=Path(self.tmp.name)
  self.ssh=self.p/'ssh';self.ssh.write_text(MOCK);self.ssh.chmod(0o755)
  self.env=dict(os.environ,OMARCHY_SSH=str(self.ssh),HOST_FIX=str(self.p),ENTRY='host-fixture',WAIT_SECONDS='10',
                HEALTH_GATE_ARGS='--min-uptime 0 --min-observe 0 --interval 0')
 def tearDown(self):self.tmp.cleanup()
 def runhost(self,**env):
  return subprocess.run([str(HERE/'kexec-host.sh'),str(image())],
                        env=dict(self.env,**env),text=True,capture_output=True)
 def test_waits_for_new_id_and_preserves_default(self):
  r=self.runhost();self.assertEqual(r.returncode,0,r.stderr)
  self.assertIn('22222222-2222',r.stdout)
  self.assertEqual((self.p/'count').read_text(),'2')
  self.assertTrue((self.p/'uploaded/Image.gz').exists())
  self.assertTrue((self.p/'uploaded/SHA256SUMS').exists())
  self.assertNotIn('set-default',(self.p/'calls').read_text())
 def test_failed_load_never_requests_reboot(self):
  r=self.runhost(FAIL_LOAD='1');self.assertNotEqual(r.returncode,0)
  self.assertFalse((self.p/'reboot-requested').exists())
 def test_timeout_is_failure(self):
  r=self.runhost(WAIT_SECONDS='1',OLD_ONLY='1');self.assertNotEqual(r.returncode,0)
  self.assertIn('timeout:',r.stderr)
 def test_default_only_after_success(self):
  r=self.runhost(SET_DEFAULT='1');self.assertEqual(r.returncode,0,r.stderr)
  calls=(self.p/'calls').read_text().splitlines()
  self.assertIn('set-default host-fixture',calls[-1])
  self.assertEqual((self.p/'probes').read_text(),'2')
 def test_unhealthy_boot_exits_3_and_keeps_default(self):
  r=self.runhost(SET_DEFAULT='1',HEALTH_FIX='frozen');self.assertEqual(r.returncode,3,r.stderr)
  self.assertIn('lost MSI',r.stderr);self.assertIn('UNHEALTHY BOOT host-fixture',r.stderr)
  self.assertNotIn('set-default',(self.p/'calls').read_text())
 def test_health_gate_can_be_skipped(self):
  r=self.runhost(HEALTH_GATE='0');self.assertEqual(r.returncode,0,r.stderr)
  self.assertFalse((self.p/'probes').exists())

if __name__=='__main__':unittest.main(verbosity=2)
