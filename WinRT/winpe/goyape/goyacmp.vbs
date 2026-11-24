' goyacmp.vbs <file1> <file2> - byte compare for WinPE (no fc.exe / comp.exe in the image).
' Reads both files in 1 MiB pieces; for the first 20 differing pieces prints the first differing offset.
' Exit code 0 = identical, 1 = different, 2 = cannot open.
Option Explicit
Dim fso, ta, tb, sa, sb, la, lb, off, chunk, i, diffs
Set fso = CreateObject("Scripting.FileSystemObject")
On Error Resume Next
la = fso.GetFile(WScript.Arguments(0)).Size
lb = fso.GetFile(WScript.Arguments(1)).Size
Set ta = fso.OpenTextFile(WScript.Arguments(0), 1, False, 0)
Set tb = fso.OpenTextFile(WScript.Arguments(1), 1, False, 0)
If Err.Number <> 0 Then WScript.Echo "goyacmp: cannot open the files: " & Err.Description : WScript.Quit 2
On Error GoTo 0
If la <> lb Then WScript.Echo "goyacmp: sizes differ: " & la & " / " & lb
chunk = 1048576 : off = 0 : diffs = 0
Do While Not ta.AtEndOfStream And Not tb.AtEndOfStream
  sa = ta.Read(chunk) : sb = tb.Read(chunk)
  If sa <> sb Then
    diffs = diffs + 1
    If diffs <= 20 Then
      For i = 1 To Len(sa)
        If Mid(sa, i, 1) <> Mid(sb, i, 1) Then Exit For
      Next
      WScript.Echo "goyacmp: MiB " & (off \ chunk) & " differs, first at offset " & (off + i - 1) & " (0x" & Hex(off + i - 1) & ")"
    End If
  End If
  off = off + Len(sa)
Loop
WScript.Echo "goyacmp: compared " & off & " bytes, " & diffs & " MiB pieces differ"
If diffs = 0 And la = lb Then WScript.Quit 0 Else WScript.Quit 1
