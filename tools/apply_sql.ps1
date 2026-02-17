param(
  [Parameter(Mandatory=$true)][string]$Database,   # acore_world | acore_characters | acore_auth
  [Parameter(Mandatory=$true)][string]$File  # path to .sql
)
Get-Content -Raw $File | mysql --login-path=acore $Database
