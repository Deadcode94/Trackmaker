@echo off
echo ========================================
echo   TRACKMAKER BUILD AND TEST SCRIPT
echo ========================================
echo.

echo [1/3] Building main application (trackmaker_fix.exe)...
cl /EHsc /nologo /Fe:trackmaker_fix.exe shared\*.cpp
if %ERRORLEVEL% neq 0 (
    echo ERROR: Main application build failed!
    pause
    exit /b %ERRORLEVEL%
)
echo Main application built successfully.
echo.

echo [2/3] Building test suite (run_tests.exe)...
cl /EHsc /nologo /Fe:run_tests.exe main_test.cpp shared\Vec4.cpp shared\AnchorPoint.cpp
if %ERRORLEVEL% neq 0 (
    echo ERROR: Test suite build failed!
    pause
    exit /b %ERRORLEVEL%
)
echo Test suite built successfully.
echo.

echo [3/3] Running test suite...
echo ----------------------------------------
run_tests.exe
if %ERRORLEVEL% neq 0 (
    echo ----------------------------------------
    echo ERROR: One or more tests failed! Check the assertion output above.
    pause
    exit /b %ERRORLEVEL%
)
echo ----------------------------------------
echo.
echo ALL TASKS COMPLETED SUCCESSFULLY!
echo Press any key to exit...
pause >nul