import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class MetalioReleaseWorkflowTest(unittest.TestCase):
    def test_manual_release_follows_the_local_package_flow(self):
        workflow = (ROOT / '.github/workflows/metalio-eink4-release.yml').read_text()
        self.assertIn('workflow_dispatch:', workflow)
        self.assertNotIn('\n  pull_request:', workflow)
        self.assertNotIn('\n  schedule:', workflow)
        self.assertIn('pio run -e metalio_eink4_nightly', workflow)
        self.assertIn('CROSSPOINT_RC_HASH=${GITHUB_SHA::7}', workflow)
        self.assertIn('python3 scripts/package_nightly_target.py metalio_eink4 \\', workflow)
        self.assertIn('--channel nightly', workflow)
        self.assertIn('--output dist/metalio_eink4', workflow)
        self.assertIn('(cd dist/metalio_eink4 && sha256sum --check *-SHA256SUMS)', workflow)
        self.assertIn('gh release create "$tag"', workflow)
        self.assertIn('--prerelease', workflow)
        self.assertNotIn('build_nightly_index.py', workflow)
        self.assertNotIn('gh release upload nightly', workflow)
        self.assertNotIn("gh release edit", workflow)
