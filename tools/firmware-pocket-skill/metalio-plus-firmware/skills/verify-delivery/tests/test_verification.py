"""Independent byte fixtures for the public Plus binding and evidence boundary.

Fixtures are parser inputs, not flashable firmware or real-device evidence.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'scripts'))
import firmware_tools as ft
import validation_record as vr


def descriptor(exit_method=1, version='9.2.0', storage=0, schema=0):
    raw = bytearray(512)
    raw[:16] = b'FPOCKET-ABI-0001'
    struct.pack_into('<HHIIBBBBI', raw, 16, 1, 0, 512, 2, storage, exit_method, 0, 0, schema)
    for pos, text in [(36, 'metalio-eink4-plus'), (68, 'metalio16m-plus-pocket2'),
                      (100, 'fixture_app'), (132, version), (164, '验收夹具'), (324, '点击返回')]:
        encoded = text.encode('utf-8')
        raw[pos:pos + len(encoded)] = encoded
    return raw


def esp(payload):
    header = bytearray(24)
    header[:4] = bytes([0xe9, 1, 2, 0x4f])
    struct.pack_into('<H', header, 12, 32)
    header[23] = 1
    raw = header + struct.pack('<II', 0x3c000020, len(payload)) + payload
    checksum = 0xef
    for byte in payload:
        checksum ^= byte
    raw += bytes(15 - len(raw) % 16) + bytes([checksum])
    return bytes(raw + hashlib.sha256(raw).digest())


def full_image():
    prefix = bytearray(b'\xff' * 0x10000)
    boot = esp(b'\0' * 4)
    prefix[0x2000:0x2000 + len(boot)] = boot
    table = bytearray()
    for name, kind, subtype, offset, size in [('nvs', 1, 2, 0x9000, 0x4000),
        ('otadata', 1, 0, 0xd000, 0x2000), ('phy_init', 1, 1, 0xf000, 0x1000),
        ('factory', 0, 0, 0x10000, 0xd00000)]:
        table += struct.pack('<HBBII16sI', 0x50aa, kind, subtype, offset, size, name.encode(), 0)
    table += b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(table).digest()
    prefix[0x8000:0x8000 + len(table)] = table
    payload = bytearray(768)
    struct.pack_into('<I', payload, 0, 0xabcd5432)
    for pos, text in [(16, '9.2.0'), (48, 'fixture_app'), (112, '6.1.0')]:
        payload[pos:pos + len(text)] = text.encode()
    payload[256:] = descriptor()
    return bytes(prefix) + esp(payload)


class VerificationTests(unittest.TestCase):
    def test_standard_exits_and_versions(self):
        for version in ['1.0.0', '9.2.0', 'future+build.23']:
            for method in range(1, 6):
                self.assertEqual(ft.metadata(descriptor(method, version))['exit_method'], method)
        with self.assertRaises(ValueError):
            ft.metadata(descriptor(6))
        with self.assertRaises(ValueError):
            ft.metadata(descriptor(storage=1, schema=0x80000000))

    def test_binary_boundaries(self):
        data = full_image()
        result = ft.inspect(data, True)
        self.assertEqual(result['applications'][0]['pocket']['version'], '9.2.0')
        corrupted = bytearray(data)
        corrupted[-1] ^= 1
        with self.assertRaises(ValueError):
            ft.inspect(corrupted, True)
        private_prefix = bytearray(data)
        private_prefix[0x9000] = 0
        with self.assertRaises(ValueError):
            ft.inspect(private_prefix, True)
        with self.assertRaises(ValueError):
            ft.inspect(data + b'extra', True)

    def test_declared_exit_generation(self):
        with tempfile.TemporaryDirectory(dir=OUTPUT) as temp:
            root = Path(temp)
            declaration = dict(app_id='example_app', version='1.0.0', name='测试', description='',
                storage_mode='NONE', state_schema=0, exit_hint='长按退出',
                tf_required_standalone=False, exit_method='AI_HOLD')
            source = root / 'input.json'
            source.write_text(json.dumps(declaration), encoding='utf-8')
            result = ft.generate_metadata(source, root / 'metadata.c')
            self.assertEqual(result['declaration']['exit_method'], 4)

    def test_evidence_is_not_hardware_pass(self):
        with tempfile.TemporaryDirectory(dir=OUTPUT) as temp:
            root = Path(temp)
            binary, record = root / 'fixture.bin', root / 'validation.json'
            binary.write_bytes(full_image())
            doc = vr.initialize(binary, record, '0.3.0', 'fixture-only', wifi=True)
            self.assertIsNone(doc['integration_host']['version'])
            self.assertFalse(vr.check_record(binary, record)['claims_complete'])
            self.assertIn('wifi_shared', doc['checks'])
            doc['checks']['pocket_boot_confirm'].update(status='pass', observed='a claim')
            record.write_text(json.dumps(doc), encoding='utf-8')
            with self.assertRaises(ValueError):
                vr.check_record(binary, record)
            doc['checks']['pocket_boot_confirm']['status'] = 'pending'
            del doc['checks']['wifi_shared']
            record.write_text(json.dumps(doc), encoding='utf-8')
            with self.assertRaises(ValueError):
                vr.check_record(binary, record)
            binary.write_bytes(binary.read_bytes() + b'x')
            with self.assertRaises(ValueError):
                vr.check_record(binary, record)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    OUTPUT = args.output.resolve()
    skill = Path(__file__).resolve().parents[3]
    if OUTPUT == skill or skill in OUTPUT.parents:
        raise SystemExit('Evidence belongs outside the Skill')
    OUTPUT.mkdir(parents=True, exist_ok=True)
    stream = io.StringIO()
    result = unittest.TextTestRunner(stream=stream, verbosity=2).run(
        unittest.defaultTestLoader.loadTestsFromTestCase(VerificationTests))
    text = stream.getvalue()
    (OUTPUT / 'verification-tests.log').write_text(text, encoding='utf-8')
    (OUTPUT / 'verification-tests.json').write_text(json.dumps({
        'passed': result.wasSuccessful(), 'tests': result.testsRun,
        'scope': 'host parser fixtures and evidence checks; no hardware validation'}, indent=2), encoding='utf-8')
    print(text)
    raise SystemExit(0 if result.wasSuccessful() else 1)
