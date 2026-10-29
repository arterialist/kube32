# Security policy

## Supported versions

Until the first tagged release, only the `main` branch receives security updates.

| Version | Supported |
| --- | --- |
| `main` | Yes |

## Deployment model

The API server and ingress serve plain HTTP. Bearer-token authentication is available for the API server and is disabled by default. Run kube32 clusters on trusted networks only.

## Reporting a vulnerability

Do not report security vulnerabilities in public GitHub issues.

Report vulnerabilities privately through GitHub Security Advisories. Open the repository Security tab and select Report a vulnerability.

Include the following details in your report:

- Affected component and commit hash.
- Configuration required to trigger the issue.
- Steps or a minimal manifest to reproduce the issue.
- Impact of the vulnerability.

You will receive an initial response within 7 days.
