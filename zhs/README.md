# zhs — Hotload Update Server

A lightweight HTTP server that hosts versioned shared-library artifacts for the zhotload system.

## Usage

```
zhs [--data DIR] [--port PORT]
```

| Flag         | Description                                | Default |
|--------------|--------------------------------------------|---------|
| `--data DIR` | Root directory for application artifacts    | `data`  |
| `--port PORT`| HTTP listen port                           | `8080`  |

## Data Directory Layout

```
data/
  {app_name}/
    {version}/
      artifact.so    (or .dylib / .dll)
```

### Registering a New Application

1. Create a directory under the data root named after your application:
   ```
   mkdir -p data/myapp/1.0.0
   ```

2. Place the shared library artifact inside the version directory:
   ```
   cp libmyapp.so data/myapp/1.0.0/
   ```

### Adding a New Version

1. Create a new version subdirectory:
   ```
   mkdir -p data/myapp/1.1.0
   ```

2. Place the updated artifact inside:
   ```
   cp libmyapp.so data/myapp/1.1.0/
   ```

The server automatically discovers versions by scanning subdirectories and selects the latest by semantic version comparison.

## HTTP Endpoints

### `GET /apps/{app_name}/latest`

Returns JSON describing the latest available version:

```json
{
  "version": "1.1.0",
  "download_url": "/apps/myapp/download/1.1.0",
  "checksum": ""
}
```

### `GET /apps/{app_name}/download/{version}`

Returns the raw shared-library binary for the specified version.
