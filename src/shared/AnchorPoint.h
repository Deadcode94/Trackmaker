<<<<<<< HEAD
//
//  Point.h
//  Trackmaker
//
//  Created by Torsten Kammer on 14.10.13.
//  Copyright (c) 2013 Torsten Kammer. All rights reserved.
//

#ifndef __Trackmaker__Point__
#define __Trackmaker__Point__

#include "Vec4.h"

class CoordinateSystem;

class AnchorPoint {
	float x, y, z;
	float direction;
	float inclination;
	float bank;
	float scaleWidth;
	float scaleHeight;
	float nextControlPointDistanceFactor;
	float previousControlPointDistanceFactor;

public:
	AnchorPoint() : x(0), y(0), z(0), direction(0), inclination(0), bank(0), 
					scaleWidth(1.0f), scaleHeight(1.0f), 
					nextControlPointDistanceFactor(0.3f), previousControlPointDistanceFactor(0.3f) {}

	void setPosition(float aX, float aY, float aZ) {
		x = aX;
		y = aY;
		z = aZ;
	}
	void getPosition(float &aX, float &aY, float &aZ) const {
		aX = x;
		aY = y;
		aZ = z;
	}
	
	void setDirection(float angleInRadian) { direction = angleInRadian; }
	float getDirection() const { return direction; }
	
	void setInclination(float angleInRadian) { inclination = angleInRadian; }
	float getInclination() const { return inclination; }
	
	void setBank(float angleInRadian) { bank = angleInRadian; }
	float getBank() const { return bank; }
	
	void setScaleWidth(float scale) { scaleWidth = scale; }
	float getScaleWidth() const { return scaleWidth; }
	
	void setScaleHeight(float scale) { scaleHeight = scale; };
	float getScaleHeight() const { return scaleHeight; }
	
	void setNextControlPointDistanceFactor(float factor) { nextControlPointDistanceFactor = factor; }
	float getNextControlPointDistanceFactor() const { return nextControlPointDistanceFactor; }
	
	void setPreviousControlPointDistanceFactor(float factor) { previousControlPointDistanceFactor = factor; }
	float getPreviousControlPointDistanceFactor() const { return previousControlPointDistanceFactor; }
	
	matrix getMatrix() const;
};

#endif /* defined(__Trackmaker__Point__) */
=======
//
//  Point.h
//  Trackmaker
//
//  Created by Torsten Kammer on 14.10.13.
//  Copyright (c) 2013 Torsten Kammer. All rights reserved.
//

#ifndef __Trackmaker__Point__
#define __Trackmaker__Point__

#include "Vec4.h"

class CoordinateSystem;

class AnchorPoint {
	float x, y, z;
	float direction;
	float inclination;
	float bank;
	float scaleWidth;
	float scaleHeight;
	float nextControlPointDistanceFactor;
	float previousControlPointDistanceFactor;

public:
	AnchorPoint() : x(0), y(0), z(0), direction(0), inclination(0), bank(0), 
					scaleWidth(1.0f), scaleHeight(1.0f), 
					nextControlPointDistanceFactor(0.3f), previousControlPointDistanceFactor(0.3f) {}

	void setPosition(float aX, float aY, float aZ) {
		x = aX;
		y = aY;
		z = aZ;
	}
	void getPosition(float &aX, float &aY, float &aZ) const {
		aX = x;
		aY = y;
		aZ = z;
	}
	
	void setDirection(float angleInRadian) { direction = angleInRadian; }
	float getDirection() const { return direction; }
	
	void setInclination(float angleInRadian) { inclination = angleInRadian; }
	float getInclination() const { return inclination; }
	
	void setBank(float angleInRadian) { bank = angleInRadian; }
	float getBank() const { return bank; }
	
	void setScaleWidth(float scale) { scaleWidth = scale; }
	float getScaleWidth() const { return scaleWidth; }
	
	void setScaleHeight(float scale) { scaleHeight = scale; };
	float getScaleHeight() const { return scaleHeight; }
	
	void setNextControlPointDistanceFactor(float factor) { nextControlPointDistanceFactor = factor; }
	float getNextControlPointDistanceFactor() const { return nextControlPointDistanceFactor; }
	
	void setPreviousControlPointDistanceFactor(float factor) { previousControlPointDistanceFactor = factor; }
	float getPreviousControlPointDistanceFactor() const { return previousControlPointDistanceFactor; }
	
	matrix getMatrix() const;
};

#endif /* defined(__Trackmaker__Point__) */
>>>>>>> 2147e2d76ea80437e46a4f8ad037ef57b7cffbbc
