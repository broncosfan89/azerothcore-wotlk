param(
  [Parameter(Mandatory=$true)][string]$Db,   # acore_world | acore_characters | acore_auth
  [Parameter(Mandatory=$true)][string]$File  # path to .sql
)
mysql --login-path=acore $Db < $File
