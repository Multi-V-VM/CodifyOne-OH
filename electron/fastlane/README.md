# Fastlane CI for HarmonyOS Electron

This directory contains the Fastlane configuration for building HarmonyOS Electron apps.

## Setup

### Prerequisites

1. Install Ruby (recommended: 3.2+)
2. Install bundler: `gem install bundler`
3. Install fastlane dependencies: `bundle install`

### Environment Variables

Set the following environment variables (optional):

```bash
# HarmonyOS Command Line Tools repository path
export HM_COMMAND_LINE_TOOLS_REPO="/path/to/harmony/tools"

# DingTalk webhook for notifications (optional)
export DINGTALK_WEBHOOK="https://oapi.dingtalk.com/robot/send?access_token=xxx"

# Deploy server credentials (if using deploy lane)
export DEPLOY_SERVER="user@host"
export DEPLOY_KEY="/path/to/key"
```

## Available Lanes

### build_hap
Build HAP package for testing/distribution

```bash
bundle exec fastlane build_hap
```

Options:
- `product` - Product variant (default: "default")
- `build_mode` - Build mode (default: "release")

### build_app
Build APP package for app store distribution

```bash
bundle exec fastlane build_app
```

Options:
- `product` - Product variant (default: "default")
- `build_mode` - Build mode (default: "release")

### deploy
Build and upload to distribution server

```bash
bundle exec fastlane deploy remote_path:"internal/harmony" download_url:"https://example.com/download"
```

### test
Run test suite

```bash
bundle exec fastlane test
```

## GitHub Actions CI

The project includes a GitHub Actions workflow that:

1. Triggers on push/PR to main branches
2. Supports manual workflow dispatch
3. Builds HAP/APP artifacts
4. Uploads build artifacts
5. Supports deployment to external servers

### Manual Workflow Trigger

Go to Actions → Fastlane CI → Run workflow → Select task

## Command Line Tools

The Fastlane setup supports dynamic Command Line Tools switching. 

To add a new version:
1. Download the Command Line Tools zip file
2. Place it in your `HM_COMMAND_LINE_TOOLS_REPO` directory
3. Update `COMMAND_LINE_TOOLS_VERSION` in Fastfile
4. Update the zip filename pattern

## Output Artifacts

Build outputs are automatically renamed with version numbers:

```
hm_fastlane_demo_unsigned_1.0.0.hap
```

## Troubleshooting

### "hvigorw not found"
- Ensure Command Line Tools are in PATH
- Check `HM_COMMAND_LINE_TOOLS_REPO` environment variable

### Build fails with signing error
- Add signing credentials to environment variables or GitHub Secrets
- Or use unsigned builds for testing

### Fastlane lane not found
- Run `bundle exec fastlane lanes` to list available lanes
- Ensure you're in the `electron/` directory

## References

- [Fastlane Documentation](https://docs.fastlane.tools)
- [HarmonyOS Build Guide](https://developer.huawei.com/consumer/cn/doc/harmonyos-guides)
