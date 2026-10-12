# SyncVersion.ps1
# Package.appxmanifest 의 Version 을 읽어 PlainPDFViewer.rc 에 동기화한다.

$ErrorActionPreference = 'Stop'

$manifestPath = Join-Path $PSScriptRoot '..\PlainPDFViewerPackage\Package.appxmanifest'
$rcPath        = Join-Path $PSScriptRoot 'PlainPDFViewer.rc'

# --- manifest 에서 버전 읽기 ---
[xml]$manifest = Get-Content $manifestPath -Encoding UTF8
$verStr = $manifest.Package.Identity.Version   # e.g. "0.3.0.0"

$parts = $verStr -split '\.'
if ($parts.Count -ne 4) {
	Write-Error "Unexpected version format in manifest: $verStr"
	exit 1
}
$a,$b,$c,$d = $parts

# rc 용 콤마 형식 (e.g. 0,3,0,0)
$verComma = "$a,$b,$c,$d"
# 문자열 형식 (e.g. 0.3.0.0)
$verDot   = $verStr

# --- rc 파일 갱신 ---
$rc = Get-Content $rcPath -Encoding UTF8

# 안전장치: 읽은 내용이 비정상(비어있거나 FILEVERSION이 없음)이면
# 절대 덮어쓰지 않고 즉시 중단한다 (파일이 잠겨 있거나 VS가 저장 중일 때
# Get-Content가 빈 내용을 반환해 원본을 파괴하는 사고를 방지)
$rcText = $rc -join "`n"
if ([string]::IsNullOrWhiteSpace($rcText) -or ($rcText -notmatch 'FILEVERSION')) {
	Write-Error "PlainPDFViewer.rc 내용을 정상적으로 읽지 못했습니다 (비어있거나 손상됨). 다른 편집기에서 열려 있는지 확인 후 다시 시도하세요. 파일을 덮어쓰지 않고 중단합니다."
	exit 1
}

# 이미 원하는 버전과 동일하면 아무 것도 하지 않는다 (멱등성 보장).
# 같은 파일에 대해 여러 프로젝트(빌드 종속성)에서 이 스크립트가 거의 동시에
# 실행되어도, 실제로 바꿀 내용이 없으면 파일에 손대지 않아 잠금 충돌을 피한다.
if (($rcText -match "FILEVERSION\s+$([regex]::Escape($verComma))\s*(?:\r?\n)") -and
	($rcText -match [regex]::Escape("`"$verDot`""))) {
	Write-Host "Version already in sync: $verDot (skip)"
	exit 0
}

$rc = $rc -replace '(?m)^ ?FILEVERSION [\d,\s]+',     "FILEVERSION $verComma"
$rc = $rc -replace '(?m)^ ?PRODUCTVERSION [\d,\s]+',  "PRODUCTVERSION $verComma"
$rc = $rc -replace '(VALUE\s+"FileVersion",\s+")[^"]+(")',   "`${1}$verDot`${2}"
$rc = $rc -replace '(VALUE\s+"ProductVersion",\s+")[^"]+(")', "`${1}$verDot`${2}"

# 치환 후 결과도 재검증 후 저장
$rcTextOut = $rc -join "`n"
if ([string]::IsNullOrWhiteSpace($rcTextOut) -or ($rcTextOut -notmatch 'FILEVERSION')) {
	Write-Error "치환 결과가 비정상적입니다. 파일을 덮어쓰지 않고 중단합니다."
	exit 1
}

# 다른 프로젝트(예: 패키징 프로젝트)가 거의 동시에 같은 스크립트를 실행해
# 파일이 잠깐 잠길 수 있으므로, 짧게 재시도한다.
$maxRetry = 5
$written = $false
for ($i = 0; $i -lt $maxRetry; $i++) {
	try {
		Set-Content $rcPath $rc -Encoding UTF8
		$written = $true
		break
	}
	catch {
		Start-Sleep -Milliseconds 300
	}
}
if (-not $written) {
	Write-Error "PlainPDFViewer.rc 쓰기에 반복 실패했습니다 (파일이 계속 잠겨 있음)."
	exit 1
}

Write-Host "Version synced: $verDot"
