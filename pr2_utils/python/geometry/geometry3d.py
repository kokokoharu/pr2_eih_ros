import time
import numpy as np

import roslib
roslib.load_manifest('tfx')
import tfx
import tf.transformations as tft

import geometry2d
import utils

import IPython

epsilon = 1e-5

class Point:
    """ Allows comparing 3d points """
    def __init__(self, p):
        """
        :param p: list/np.array
        """
        self.p = np.array(p)
        
    def __eq__(self, other):
        """
        :param other: list/np.array/Point
        """
        if isinstance(other, Point):
            return np.linalg.norm(self.p - other.p) < epsilon
        elif isinstance(other, list) or isinstance(other, np.array):
            return np.linalg.norm(self.p - np.array(other)) < epsilon
        return False
    
    def __hash__(self):
        return 0
        

class Pyramid:
    def __init__(self, base, a, b, c):
        """
        A pyramid with orign base and points a,b,c
        """
        self.base = np.array(base)
        self.a = np.array(a)
        self.b = np.array(b)
        self.c = np.array(c)
        
    def is_inside(self, p):
        """
        :param p: 3d point as list or np.array
        :return True if p is inside the pyramid, else False
        """
        p = np.array(p)
        
        halfspaces = self.halfspaces
        return np.min([h.contains(p) for h in halfspaces])    
    
    def signed_distance(self, point):
        """
        :param 3d point as list or np.array
        :return float signed-distance
        """
        point = np.array(point)
        
        sign = -1 if self.is_inside(point) else 1
        dist = min([tri.distance_to(point) for tri in self.faces])
        
        return sign*dist
    
    @property
    def halfspaces(self):
        """
        :return list of halfspaces representing outward-pointing faces
        """
        base, a, b, c = self.base, self.a, self.b, self.c

        origins = [(base+a+b)/3.,
                   (base+b+c)/3.,
                   (base+c+a)/3.,
                   (a+b+c)/3.]
    
        normals = [np.cross(a-base, b-base),
                   np.cross(b-base, c-base),
                   np.cross(c-base, a-base),
                   -np.cross(b-a, c-a)]
        normals = [n/np.linalg.norm(n) for n in normals]
        
        return [Halfspace(origin, normal) for origin, normal in zip(origins, normals)]
    
    @property
    def faces(self):
        """
        :return list of Triangle representing the faces of the pyramid
        """
        return [Triangle(self.base, self.a, self.b),
                Triangle(self.base, self.b, self.c),
                Triangle(self.base, self.c, self.a),
                Triangle(self.a, self.b, self.c)]
    
    def plot(self, sim, frame='world', fill=False, with_sides=True, color=(1,0,0), alpha=0.25):
        """
        Plots edges of the pyramid
        
        :param sim: Simulator instance
        :param frame: frame in which points are defined in
        :param fill: if True, colors the faces
        :param with_sides: if True, plots side edges too
        :param color: (r,g,b) [0,1]
        :param alpha: if fill is True, alpha of faces
        """
        base = sim.transform_from_to(self.base, frame, 'world')
        a = sim.transform_from_to(self.a, frame, 'world')
        b = sim.transform_from_to(self.b, frame, 'world')
        c = sim.transform_from_to(self.c, frame, 'world')
        
        if with_sides:
            sim.plot_segment(base, a, color=color)
            sim.plot_segment(base, b, color=color)
            sim.plot_segment(base, c, color=color)
            
        sim.plot_segment(a, b, color=color)
        sim.plot_segment(b, c, color=color)
        sim.plot_segment(c, a, color=color)
        
        if fill:
            if with_sides:
                sim.plot_triangle((base, a, b), color=color, alpha=alpha)
                sim.plot_triangle((base, b, c), color=color, alpha=alpha)
                sim.plot_triangle((base, c, a), color=color, alpha=alpha)
                
            sim.plot_triangle((a, b, c), color=color, alpha=alpha)
    
class RectangularPyramid:
    def __init__(self, base, a, b, c, d):
        """
        A pyramid with origin base and points a,b,c,d arranged as
        b --- a
        |     |
        |     |
        c --- d
        """
        self.base = np.array(base)
        self.a = np.array(a)
        self.b = np.array(b)
        self.c = np.array(c)
        self.d = np.array(d)
        
    def is_inside(self, p):
        """
        :param p: 3d point as list or np.array
        :return True if p is inside the pyramid, else False
        """
        p = np.array(p)
        
        halfspaces = self.halfspaces
        return np.min([h.contains(p) for h in halfspaces])
    
    @property
    def halfspaces(self):
        """
        :return list of halfspaces representing outward-pointing faces
        """
        base, a, b, c, d = self.base, self.a, self.b, self.c, self.d

        origins = [(base+a+d)/3.0,
                   (base+b+a)/3.0,
                   (base+c+b)/3.0,
                   (base+d+c)/3.0,
                   (a+b+c+d)/4.0]
    
        normals = [np.cross(a-base, d-base),
                   np.cross(b-base, a-base),
                   np.cross(c-base, b-base),
                   np.cross(d-base, c-base),
                   np.cross(b-a, d-a)]
        normals = [n/np.linalg.norm(n) for n in normals]
        
        return [Halfspace(origin, normal) for origin, normal in zip(origins, normals)]
        
    def clip_triangle(self, triangle):
        """
        Clips triangle against faces (http://www.cs.uu.nl/docs/vakken/gr/2011/Slides/08-pipeline2.pdf)
        :param triangle: Triangle
        :return list of Triangle
        """
        triangles = [triangle]
        for halfspace in self.halfspaces:
            new_triangles = list()
            # clip all triangles against the halfspace
            for tri in triangles:
                hyperplane = halfspace.hyperplane
                intersections = filter(lambda x: x is not None, [hyperplane.intersection(segment) for segment in tri.segments])
                assert len(intersections) == 0 or len(intersections) == 2
                inside_vertices = [vertex for vertex in tri.vertices if halfspace.contains(vertex)]
                if len(intersections) == 2:
                    assert len(inside_vertices) == 1 or len(inside_vertices) == 2
                    if len(inside_vertices) == 1:
                        # then intersections form new border of triangle
                        new_triangles.append(Triangle(inside_vertices[0], intersections[0], intersections[1]))
                    else:
                        # create two triangles
                        new_triangles.append(Triangle(inside_vertices[0], intersections[0], intersections[1]))
                        if np.linalg.norm(inside_vertices[1] - intersections[0]) < np.linalg.norm(inside_vertices[1] - intersections[1]):
                            new_triangles.append(Triangle(inside_vertices[1], intersections[0], inside_vertices[0]))
                        else:
                            new_triangles.append(Triangle(inside_vertices[1], intersections[1], inside_vertices[0]))
                else:
                    # all/none of triangle in halfspace
                    assert len(inside_vertices) == 0 or len(inside_vertices) == 3
                    if len(inside_vertices) == 3:
                        new_triangles.append(tri) 
                        
            triangles = new_triangles
            
        return triangles
    
    def plot(self, sim, frame='world', fill=False, with_sides=True, color=(1,0,0), alpha=0.25):
        """
        Plots edges of the pyramid
        :param sim: Simulator instance
        :param frame: frame in which points are defined in
        :param fill: if True, colors the faces
        :param with_sides: if True, plots side edges too
        :param color: (r,g,b) [0,1]
        :param alpha: if fill is True, alpha of faces
        """
        base = sim.transform_from_to(self.base, frame, 'world')
        a = sim.transform_from_to(self.a, frame, 'world')
        b = sim.transform_from_to(self.b, frame, 'world')
        c = sim.transform_from_to(self.c, frame, 'world')
        d = sim.transform_from_to(self.d, frame, 'world')
        
        if with_sides:
            sim.plot_segment(base, a, color=color)
            sim.plot_segment(base, b, color=color)
            sim.plot_segment(base, c, color=color)
            sim.plot_segment(base, d, color=color)
        
        sim.plot_segment(a, b, color=color)
        sim.plot_segment(b, c, color=color)
        sim.plot_segment(c, d, color=color)
        sim.plot_segment(d, a, color=color)
        
        if fill:
            if with_sides:
                sim.plot_triangle((base,a,b), color=color, alpha=alpha)
                sim.plot_triangle((base,b,c), color=color, alpha=alpha)
                sim.plot_triangle((base,c,d), color=color, alpha=alpha)
                sim.plot_triangle((base,d,a), color=color, alpha=alpha)
                
            sim.plot_triangle((a,b,c), color=color, alpha=alpha)
            sim.plot_triangle((a,c,d), color=color, alpha=alpha)
    
class Triangle:
    def __init__(self, a, b, c):
        self.a, self.b, self.c = np.array(a), np.array(b), np.array(c)
        
    def align_with(self, target):
        """
        Aligns the normal of this triangle to target
        
        :param target: 3d list or np.array
        :return (rotated triangle, rotation matrix)
        """
        target = np.array(target)
        source = np.cross(self.b - self.a, self.c - self.a)
        source /= np.linalg.norm(source)
    
        rotation = np.eye(3)
        
        dot = np.dot(source, target)
        if not np.isnan(dot):
            angle = np.arccos(dot)
            if not np.isnan(angle):
                cross = np.cross(source, target)
                cross_norm = np.linalg.norm(cross)
                if not np.isnan(cross_norm) and not cross_norm < epsilon:
                    cross = cross / cross_norm
                    rotation = tft.rotation_matrix(angle, cross)[:3,:3]

        return (Triangle(np.dot(rotation, self.a),
                        np.dot(rotation, self.b),
                        np.dot(rotation, self.c)),
                rotation)
        
    def closest_point_to(self, p):
        """
        Find distance to point p
        by rotating and projecting
        then return that closest point unrotated
        
        :param p: 3d list or np.array
        :return 3d np.array of closest point
        """
        p = np.array(p)
        # align with z-axis so all triangle have same z-coord
        tri_rot, rot = self.align_with([0,0,1])
        tri_rot_z = tri_rot.a[-1]
        p_rot = np.dot(rot, p)
        
        p_2d = p_rot[:2]
        tri_2d = geometry2d.Triangle(tri_rot.a[:2], tri_rot.b[:2], tri_rot.c[:2])
        
        if tri_2d.is_inside(p_2d):
            # projects onto triangle, so return difference in z
            return np.dot(np.linalg.inv(rot), np.array(list(p_2d) + [tri_rot_z]))
        else:
            closest_pt_2d = tri_2d.closest_point_to(p_2d)
            
            closest_pt_3d = np.array(list(closest_pt_2d) + [tri_rot_z])
            
            return np.dot(np.linalg.inv(rot), closest_pt_3d)
        
    def distance_to(self, p):
        """
        Find distance to point p
        by rotating and projecting
        
        :param p: 3d list or np.array
        :return float distance
        """
        closest_pt = self.closest_point_to(p)
        return np.linalg.norm(p - closest_pt)
    
    def intersection(self, segment):
        """
        Determine point where line segment intersects this triangle
        - find intersection of segment with hyperplane
        - if intersection is in the triangle, return it
        
        :param segment: 3d line segment
        :return 3d np.array if intersection, else None
        """
        intersection = self.hyperplane.intersection(segment)
        if intersection is not None and np.linalg.norm(intersection - self.closest_point_to(intersection)) < epsilon:
            return intersection
        
        return None
    
    @property
    def vertices(self):
        """
        :return list of np.array points
        """
        return [self.a, self.b, self.c]
        
    @property
    def segments(self):
        """
        :return list of Segment
        """
        return [Segment(self.a, self.b), Segment(self.b, self.c), Segment(self.c, self.a)]
        
    @property
    def hyperplane(self):
        """
        Returns hyperplane that this triangle is embedded in
        
        :return Hyperplane
        """
        origin = (self.a+self.b+self.c)/3.
        normal = np.cross(self.a-self.b, self.a-self.c)
        return Hyperplane(origin, normal)
    
    @property
    def area(self):
        """
        :return area of the triangle
        """
        tri_rot, rot = self.align_with([0,0,1])
        tri_2d = geometry2d.Triangle(tri_rot.a[:2], tri_rot.b[:2], tri_rot.c[:2])
        return tri_2d.area
        
    def plot(self, sim, frame='world', fill=False, color=(1,0,0), alpha=0.25):
        """
        :param sim: Simulator instance
        :param fill: if True, colors the faces
        :param frame: frame in which points are defined in
        :param color: (r,g,b) [0,1]
        :param alpha: if fill is True, alpha of faces
        """
        a = sim.transform_from_to(self.a, frame, 'world')
        b = sim.transform_from_to(self.b, frame, 'world')
        c = sim.transform_from_to(self.c, frame, 'world')
        
        sim.plot_segment(a, b, color)
        sim.plot_segment(b, c, color)
        sim.plot_segment(c, a, color)
        
        if fill:
            sim.plot_triangle((a,b,c), color=color, alpha=alpha)
    
class Segment:
    def __init__(self, p0, p1):
        self.p0, self.p1 = np.array(p0), np.array(p1)
        
    def closest_point_to(self, x):
        """
        min_{0<=t<=1} ||t*(p1-p0) + p0 - x||_{2}^{2}
        
        :param x: 3d list or np.array
        :return 3d np.array of closest point on segment to x
        """
        x = np.array(x)
        v = self.p1 - self.p0
        b = self.p0 - x
        
        t = -np.dot(v, b) / np.dot(v, v)
        if (0 <= t <= 1):
            closest = t*(self.p1 - self.p0) + self.p0
            return closest
        else:
            if np.linalg.norm(x - self.p0) < np.linalg.norm(x - self.p1):
                return self.p0
            else:
                return self.p1
            
    def intersection(self, other):
        """
        Finds intersection point with another segment
        :param other: Segment
        :return None if no intersection, else [x,y] of intersection 
        """
        p0_other, p1_other = other.p0, other.p1
        
        # w = p1 - p0
        # v = p1_other - p0_other
        # s*w + p0 = t*v + p_other
        
        w = self.p1 - self.p0
        v = p1_other - p0_other
        
        A = np.vstack((w,v)).T
        b = p0_other - self.p0
        
        if np.abs(np.linalg.det(A)) < epsilon:
            return None
        
        soln = np.linalg.solve(A, b)
        s, t = soln[0], -soln[1]
        
        intersection = s*w + self.p0
        
        if ((-epsilon <= s) and (s <= 1+epsilon) and (-epsilon <= t) and (t <= 1+epsilon)):
            return intersection
        else:
            return None
            
    def plot(self, sim, frame='world', color=(1,0,0)):
        """
        :param axes: pyplot axes
        :param frame: frame in which points are defined in
        :param color: character or (r,g,b) [0,1]
        """
        p0 = sim.transform_from_to(self.p0, frame, 'world')
        p1 = sim.transform_from_to(self.p1, frame, 'world')
        sim.plot_segment(p0, p1, color=color)
        
    
class Halfspace:
    def __init__(self, origin, normal):
        self.origin = origin
        self.normal = normal
        
    def contains(self, x):
        """
        :param x: 3d point as list or np.array
        :return True if x forms acute angle with plane normal, else False
        """
        return np.dot(self.normal, np.array(x) - self.origin) >= epsilon
    
    @property
    def hyperplane(self):
        """
        :return Hyperplane that defines Halfspace
        """
        return Hyperplane(self.origin, self.normal)
    
    def plot(self, sim, frame='world', color=(0,0,1)):
        """
        Plots the normal
        
        :param sim: Simulator instance
        :param frame: frame in which points are defined in
        :param color: (r,g,b) [0,1]
        """
        o = sim.transform_from_to(self.origin, frame, 'world')
        n = sim.transform_from_to(self.normal, frame, 'world')
        sim.plot_segment(o, o + .05*n, color=color)
    
class Hyperplane:
    def __init__(self, origin, normal):
        self.origin = origin
        self.normal = normal
        
    def intersection(self, segment):
        """
        Finds intersection with a line segment
        
        :segment segment: 2d line segment
        :return 2d np.array, or None if no intersection
        """
        p0, p1 = segment.p0, segment.p1
        
        # x = t*(p1 - p0) + p0
        # n'*(x - origin) = 0
        # combine to get
        # n'*(t*(p1-p0) + p0 - origin) = 0
        # solve for t
        
        v = p1 - p0
        w = p0 - self.origin
        t = -np.dot(self.normal, w)/np.dot(self.normal, v)
        
        if 0 <= t <= 1:
            return t*(p1-p0) + p0
        else:
            return None
        