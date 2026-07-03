# CYD Banners Server Scripts

## Quick Start

```powershell
# Check status
.\server\status-server.ps1

# Start server
.\server\start-server.ps1

# Stop server
.\server\stop-server.ps1
```

## Notes

- `start-server.ps1` runs the server in the foreground (no new window).
- The server listens on `http://0.0.0.0:8088` by default.
- Admin UI: `http://192.168.4.2:8088/admin/`
- Content files: `http://192.168.4.2:8088/cyd-banners/files/<path>`
