<<<<<<< HEAD
#include <iostream>
#include <cassert>
#include <cmath>
#include "shared/Vec4.h"
#include "shared/AnchorPoint.h"

// Utility function to compare floating point numbers
bool isAlmostEqual(float a, float b, float epsilon = 0.001f) {
    return std::abs(a - b) < epsilon;
}

void testVectorNormalization() {
    std::cout << "Running testVectorNormalization..." << std::endl;

    float4 vec(3.0f, 4.0f, 0.0f, 0.0f); // Length should be 5
    float4 normalizedVec = vec.normalized();

    assert(isAlmostEqual(normalizedVec.length(), 1.0f) && "Normalization failed: length is not 1.0");
    assert(isAlmostEqual(normalizedVec.x, 0.6f) && "Normalization failed on X axis");
    assert(isAlmostEqual(normalizedVec.y, 0.8f) && "Normalization failed on Y axis");

    std::cout << "testVectorNormalization PASSED!" << std::endl;
}

void testCrossProduct() {
    std::cout << "Running testCrossProduct..." << std::endl;

    // X axis cross Y axis should result in Z axis
    float4 xAxis(1.0f, 0.0f, 0.0f, 0.0f);
    float4 yAxis(0.0f, 1.0f, 0.0f, 0.0f);

    float4 zAxisResult = xAxis.cross(yAxis);

    assert(isAlmostEqual(zAxisResult.z, 1.0f) && "Cross product failed: Z should be 1.0");
    assert(isAlmostEqual(zAxisResult.x, 0.0f) && "Cross product failed: X should be 0.0");
    assert(isAlmostEqual(zAxisResult.y, 0.0f) && "Cross product failed: Y should be 0.0");

    std::cout << "testCrossProduct PASSED!" << std::endl;
}

void testMatrixMultiplication() {
    std::cout << "Running testMatrixMultiplication..." << std::endl;

    matrix identity;
    identity.x = float4(1.0f, 0.0f, 0.0f, 0.0f);
    identity.y = float4(0.0f, 1.0f, 0.0f, 0.0f);
    identity.z = float4(0.0f, 0.0f, 1.0f, 0.0f);
    identity.w = float4(0.0f, 0.0f, 0.0f, 1.0f);

    float ninetyDegrees = 90.0f * (M_PI / 180.0f);
    matrix rotMatrix = matrix::rotation(float4(1.0f, 0.0f, 0.0f, 0.0f), ninetyDegrees);

    matrix result = identity * rotMatrix;

    assert(isAlmostEqual(result.y.z, 1.0f) && "Matrix multiplication failed on Y.z");
    assert(isAlmostEqual(result.y.y, 0.0f) && "Matrix multiplication failed on Y.y");

    std::cout << "testMatrixMultiplication PASSED!" << std::endl;
}

void testAnchorPointStorage() {
    std::cout << "Running testAnchorPointStorage..." << std::endl;

    AnchorPoint pt;
    pt.setPosition(100.0f, 200.0f, -300.0f);
    pt.setBank(45.0f * (M_PI / 180.0f));
    pt.setScaleWidth(2.5f);
    pt.setScaleHeight(0.5f);

    float px, py, pz;
    pt.getPosition(px, py, pz);

    assert(isAlmostEqual(px, 100.0f) && "AnchorPoint X position corrupted");
    assert(isAlmostEqual(py, 200.0f) && "AnchorPoint Y position corrupted");
    assert(isAlmostEqual(pz, -300.0f) && "AnchorPoint Z position corrupted");
    assert(isAlmostEqual(pt.getBank(), 45.0f * (M_PI / 180.0f)) && "AnchorPoint bank corrupted");
    assert(isAlmostEqual(pt.getScaleWidth(), 2.5f) && "AnchorPoint scale width corrupted");

    std::cout << "testAnchorPointStorage PASSED!" << std::endl;
}

void testBezierControlPoints() {
    std::cout << "Running testBezierControlPoints..." << std::endl;

    AnchorPoint startPt;
    startPt.setPosition(0.0f, 0.0f, 0.0f);
    startPt.setDirection(0.0f); // Pointing forward along X
    startPt.setNextControlPointDistanceFactor(0.5f);

    AnchorPoint endPt;
    endPt.setPosition(100.0f, 0.0f, 0.0f);

    matrix startMat = startPt.getMatrix();
    matrix endMat = endPt.getMatrix();
    float length = (startMat.w - endMat.w).length();

    // Control point 1 should be pushed forward along the X axis by half the distance
    float4 controlPoint1 = startMat.w + startMat.x * length * startPt.getNextControlPointDistanceFactor();

    assert(isAlmostEqual(controlPoint1.x, 50.0f) && "Bezier control point X calculation is wrong");
    assert(isAlmostEqual(controlPoint1.y, 0.0f) && "Bezier control point Y calculation is wrong");
    assert(isAlmostEqual(controlPoint1.z, 0.0f) && "Bezier control point Z calculation is wrong");

    std::cout << "testBezierControlPoints PASSED!" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "   TRACKMAKER CORE MATH TEST RUNNER     " << std::endl;
    std::cout << "========================================" << std::endl;

    testVectorNormalization();
    testCrossProduct();
    testMatrixMultiplication();
    testAnchorPointStorage();
    testBezierControlPoints();

    std::cout << "========================================" << std::endl;
    std::cout << "   ALL TESTS EXECUTED SUCCESSFULLY!     " << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
=======
#include <iostream>
#include <cassert>
#include <cmath>
#include "shared/Vec4.h"
#include "shared/AnchorPoint.h"

// Utility function to compare floating point numbers
bool isAlmostEqual(float a, float b, float epsilon = 0.001f) {
    return std::abs(a - b) < epsilon;
}

void testVectorNormalization() {
    std::cout << "Running testVectorNormalization..." << std::endl;

    float4 vec(3.0f, 4.0f, 0.0f, 0.0f); // Length should be 5
    float4 normalizedVec = vec.normalized();

    assert(isAlmostEqual(normalizedVec.length(), 1.0f) && "Normalization failed: length is not 1.0");
    assert(isAlmostEqual(normalizedVec.x, 0.6f) && "Normalization failed on X axis");
    assert(isAlmostEqual(normalizedVec.y, 0.8f) && "Normalization failed on Y axis");

    std::cout << "testVectorNormalization PASSED!" << std::endl;
}

void testCrossProduct() {
    std::cout << "Running testCrossProduct..." << std::endl;

    // X axis cross Y axis should result in Z axis
    float4 xAxis(1.0f, 0.0f, 0.0f, 0.0f);
    float4 yAxis(0.0f, 1.0f, 0.0f, 0.0f);

    float4 zAxisResult = xAxis.cross(yAxis);

    assert(isAlmostEqual(zAxisResult.z, 1.0f) && "Cross product failed: Z should be 1.0");
    assert(isAlmostEqual(zAxisResult.x, 0.0f) && "Cross product failed: X should be 0.0");
    assert(isAlmostEqual(zAxisResult.y, 0.0f) && "Cross product failed: Y should be 0.0");

    std::cout << "testCrossProduct PASSED!" << std::endl;
}

void testMatrixMultiplication() {
    std::cout << "Running testMatrixMultiplication..." << std::endl;

    matrix identity;
    identity.x = float4(1.0f, 0.0f, 0.0f, 0.0f);
    identity.y = float4(0.0f, 1.0f, 0.0f, 0.0f);
    identity.z = float4(0.0f, 0.0f, 1.0f, 0.0f);
    identity.w = float4(0.0f, 0.0f, 0.0f, 1.0f);

    float ninetyDegrees = 90.0f * (M_PI / 180.0f);
    matrix rotMatrix = matrix::rotation(float4(1.0f, 0.0f, 0.0f, 0.0f), ninetyDegrees);

    matrix result = identity * rotMatrix;

    assert(isAlmostEqual(result.y.z, 1.0f) && "Matrix multiplication failed on Y.z");
    assert(isAlmostEqual(result.y.y, 0.0f) && "Matrix multiplication failed on Y.y");

    std::cout << "testMatrixMultiplication PASSED!" << std::endl;
}

void testAnchorPointStorage() {
    std::cout << "Running testAnchorPointStorage..." << std::endl;

    AnchorPoint pt;
    pt.setPosition(100.0f, 200.0f, -300.0f);
    pt.setBank(45.0f * (M_PI / 180.0f));
    pt.setScaleWidth(2.5f);
    pt.setScaleHeight(0.5f);

    float px, py, pz;
    pt.getPosition(px, py, pz);

    assert(isAlmostEqual(px, 100.0f) && "AnchorPoint X position corrupted");
    assert(isAlmostEqual(py, 200.0f) && "AnchorPoint Y position corrupted");
    assert(isAlmostEqual(pz, -300.0f) && "AnchorPoint Z position corrupted");
    assert(isAlmostEqual(pt.getBank(), 45.0f * (M_PI / 180.0f)) && "AnchorPoint bank corrupted");
    assert(isAlmostEqual(pt.getScaleWidth(), 2.5f) && "AnchorPoint scale width corrupted");

    std::cout << "testAnchorPointStorage PASSED!" << std::endl;
}

void testBezierControlPoints() {
    std::cout << "Running testBezierControlPoints..." << std::endl;

    AnchorPoint startPt;
    startPt.setPosition(0.0f, 0.0f, 0.0f);
    startPt.setDirection(0.0f); // Pointing forward along X
    startPt.setNextControlPointDistanceFactor(0.5f);

    AnchorPoint endPt;
    endPt.setPosition(100.0f, 0.0f, 0.0f);

    matrix startMat = startPt.getMatrix();
    matrix endMat = endPt.getMatrix();
    float length = (startMat.w - endMat.w).length();

    // Control point 1 should be pushed forward along the X axis by half the distance
    float4 controlPoint1 = startMat.w + startMat.x * length * startPt.getNextControlPointDistanceFactor();

    assert(isAlmostEqual(controlPoint1.x, 50.0f) && "Bezier control point X calculation is wrong");
    assert(isAlmostEqual(controlPoint1.y, 0.0f) && "Bezier control point Y calculation is wrong");
    assert(isAlmostEqual(controlPoint1.z, 0.0f) && "Bezier control point Z calculation is wrong");

    std::cout << "testBezierControlPoints PASSED!" << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "   TRACKMAKER CORE MATH TEST RUNNER     " << std::endl;
    std::cout << "========================================" << std::endl;

    testVectorNormalization();
    testCrossProduct();
    testMatrixMultiplication();
    testAnchorPointStorage();
    testBezierControlPoints();

    std::cout << "========================================" << std::endl;
    std::cout << "   ALL TESTS EXECUTED SUCCESSFULLY!     " << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
>>>>>>> 2147e2d76ea80437e46a4f8ad037ef57b7cffbbc
}