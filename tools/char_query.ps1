param(
  [Parameter(Mandatory=$true)][string]$Sql
)
mysql --login-path=acore acore_characters -e $Sql
