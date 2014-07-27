/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file blender/freestyle/intern/blender_interface/BlenderFileLoader.cpp
 *  \ingroup freestyle
 */

#include "BlenderFileLoader.h"

#include "BLI_utildefines.h"

#include "BKE_global.h"

#include <sstream>

namespace Freestyle {

BlenderFileLoader::BlenderFileLoader(Render *re, SceneRenderLayer *srl)
{
	_re = re;
	_srl = srl;
	_Scene = NULL;
	_numFacesRead = 0;
	_minEdgeSize = DBL_MAX;
	_smooth = (srl->freestyleConfig.flags & FREESTYLE_FACE_SMOOTHNESS_FLAG) != 0;
	_pRenderMonitor = NULL;
}

BlenderFileLoader::~BlenderFileLoader()
{
	_Scene = NULL;
}

NodeGroup *BlenderFileLoader::Load()
{
	ObjectInstanceRen *obi;

	if (G.debug & G_DEBUG_FREESTYLE) {
		cout << "\n===  Importing triangular meshes into Blender  ===" << endl;
	}

	// creation of the scene root node
	_Scene = new NodeGroup;

	_viewplane_left =   _re->viewplane.xmin;
	_viewplane_right =  _re->viewplane.xmax;
	_viewplane_bottom = _re->viewplane.ymin;
	_viewplane_top =    _re->viewplane.ymax;

	if (_re->clipsta < 0.f) {
		// Adjust clipping start/end and set up a Z offset when the viewport preview
		// is used with the orthographic view.  In this case, _re->clipsta is negative,
		// while Freestyle assumes that imported mesh data are in the camera coordinate
		// system with the view point located at origin [bug #36009].
		_z_near = -0.001f;
		_z_offset = _re->clipsta + _z_near;
		_z_far = -_re->clipend + _z_offset;
	}
	else {
		_z_near = -_re->clipsta;
		_z_far = -_re->clipend;
		_z_offset = 0.f;
	}

#if 0
	if (G.debug & G_DEBUG_FREESTYLE) {
		cout << "Frustum: l " << _viewplane_left << " r " << _viewplane_right
		     << " b " << _viewplane_bottom << " t " << _viewplane_top
		     << " n " << _z_near << " f " << _z_far << endl;
	}
#endif

	int id = 0;
	unsigned cnt = 1;
	unsigned cntStep = (unsigned)ceil(0.01f * _re->totinstance);
	for (obi = (ObjectInstanceRen *)_re->instancetable.first; obi; obi = obi->next) {
		if (_pRenderMonitor) {
			if (_pRenderMonitor->testBreak())
				break;
			if (cnt % cntStep == 0) {
				stringstream ss;
				ss << "Freestyle: Mesh loading " << (100 * cnt / _re->totinstance) << "%";
				_pRenderMonitor->setInfo(ss.str());
				_pRenderMonitor->progress((float)cnt / _re->totinstance);
			}
			cnt++;
		}

		if (!(obi->lay & _srl->lay))
			continue;
		char *name = obi->ob->id.name;
		//printf("%c%c:%s\n", name[0], name[1], name+2);
		//print_m4("obi->mat", obi->mat);

		if (obi->obr->totvlak > 0) {
			insertShapeNode(obi, ++id);
		}
		else if (G.debug & G_DEBUG_FREESTYLE) {
			cout << "Warning: " << (name + 2) << " is not a vlak-based object (ignored)" << endl;
		}
	}

	// Return the built scene.
	return _Scene;
}

#define CLIPPED_BY_NEAR -1
#define NOT_CLIPPED      0
#define CLIPPED_BY_FAR   1

// check if each vertex of a triangle (V1, V2, V3) is clipped by the near/far plane
// and calculate the number of triangles to be generated by clipping
int BlenderFileLoader::countClippedFaces(float v1[3], float v2[3], float v3[3], int clip[3])
{
	float *v[3];
	int numClipped, sum, numTris = 0;

	v[0] = v1;
	v[1] = v2;
	v[2] = v3;
	numClipped = sum = 0;
	for (int i = 0; i < 3; i++) {
		if (v[i][2] > _z_near) {
			clip[i] = CLIPPED_BY_NEAR;
			numClipped++;
		}
		else if (v[i][2] < _z_far) {
			clip[i] = CLIPPED_BY_FAR;
			numClipped++;
		}
		else {
			clip[i] = NOT_CLIPPED;
		}
#if 0
		if (G.debug & G_DEBUG_FREESTYLE) {
			printf("%d %s\n", i, (clip[i] == NOT_CLIPPED) ? "not" : (clip[i] == CLIPPED_BY_NEAR) ? "near" : "far");
		}
#endif
		sum += clip[i];
	}
	switch (numClipped) {
	case 0:
		numTris = 1; // triangle
		break;
	case 1:
		numTris = 2; // tetragon
		break;
	case 2:
		if (sum == 0)
			numTris = 3; // pentagon
		else
			numTris = 1; // triangle
		break;
	case 3:
		if (sum == 3 || sum == -3)
			numTris = 0;
		else
			numTris = 2; // tetragon
		break;
	}
	return numTris;
}

// find the intersection point C between the line segment from V1 to V2 and
// a clipping plane at depth Z (i.e., the Z component of C is known, while
// the X and Y components are unknown).
void BlenderFileLoader::clipLine(float v1[3], float v2[3], float c[3], float z)
{
	// Order v1 and v2 by Z values to make sure that clipLine(P, Q, c, z)
	// and clipLine(Q, P, c, z) gives exactly the same numerical result.
	float *p, *q;
	if (v1[2] < v2[2]) {
		p = v1;
		q = v2;
	}
	else {
		p = v2;
		q = v1;
	}
	double d[3];
	for (int i = 0; i < 3; i++)
		d[i] = q[i] - p[i];
	double t = (z - p[2]) / d[2];
	c[0] = p[0] + t * d[0];
	c[1] = p[1] + t * d[1];
	c[2] = z;
}

// clip the triangle (V1, V2, V3) by the near and far clipping plane and
// obtain a set of vertices after the clipping.  The number of vertices
// is at most 5.
void BlenderFileLoader::clipTriangle(int numTris, float triCoords[][3], float v1[3], float v2[3], float v3[3],
                                     float triNormals[][3], float n1[3], float n2[3], float n3[3],
                                     bool edgeMarks[], bool em1, bool em2, bool em3, int clip[3])
{
	float *v[3], *n[3];
	bool em[3];
	int i, j, k;

	v[0] = v1; n[0] = n1;
	v[1] = v2; n[1] = n2;
	v[2] = v3; n[2] = n3;
	em[0] = em1; /* edge mark of the edge between v1 and v2 */
	em[1] = em2; /* edge mark of the edge between v2 and v3 */
	em[2] = em3; /* edge mark of the edge between v3 and v1 */
	k = 0;
	for (i = 0; i < 3; i++) {
		j = (i + 1) % 3;
		if (clip[i] == NOT_CLIPPED) {
			copy_v3_v3(triCoords[k], v[i]);
			copy_v3_v3(triNormals[k], n[i]);
			edgeMarks[k] = em[i];
			k++;
			if (clip[j] != NOT_CLIPPED) {
				clipLine(v[i], v[j], triCoords[k], (clip[j] == CLIPPED_BY_NEAR) ? _z_near : _z_far);
				copy_v3_v3(triNormals[k], n[j]);
				edgeMarks[k] = false;
				k++;
			}
		}
		else if (clip[i] != clip[j]) {
			if (clip[j] == NOT_CLIPPED) {
				clipLine(v[i], v[j], triCoords[k], (clip[i] == CLIPPED_BY_NEAR) ? _z_near : _z_far);
				copy_v3_v3(triNormals[k], n[i]);
				edgeMarks[k] = em[i];
				k++;
			}
			else {
				clipLine(v[i], v[j], triCoords[k], (clip[i] == CLIPPED_BY_NEAR) ? _z_near : _z_far);
				copy_v3_v3(triNormals[k], n[i]);
				edgeMarks[k] = em[i];
				k++;
				clipLine(v[i], v[j], triCoords[k], (clip[j] == CLIPPED_BY_NEAR) ? _z_near : _z_far);
				copy_v3_v3(triNormals[k], n[j]);
				edgeMarks[k] = false;
				k++;
			}
		}
	}
	BLI_assert(k == 2 + numTris);
	(void)numTris;  /* Ignored in release builds. */
}

void BlenderFileLoader::addTriangle(struct LoaderState *ls, float v1[3], float v2[3], float v3[3],
                                    float n1[3], float n2[3], float n3[3],
                                    bool fm, bool em1, bool em2, bool em3)
{
	float *fv[3], *fn[3], len;
	unsigned int i, j;
	IndexedFaceSet::FaceEdgeMark marks = 0;

	// initialize the bounding box by the first vertex
	if (ls->currentIndex == 0) {
		copy_v3_v3(ls->minBBox, v1);
		copy_v3_v3(ls->maxBBox, v1);
	}

	fv[0] = v1; fn[0] = n1;
	fv[1] = v2; fn[1] = n2;
	fv[2] = v3; fn[2] = n3;
	for (i = 0; i < 3; i++) {

		copy_v3_v3(ls->pv, fv[i]);
		copy_v3_v3(ls->pn, fn[i]);

		// update the bounding box
		for (j = 0; j < 3; j++) {
			if (ls->minBBox[j] > ls->pv[j])
				ls->minBBox[j] = ls->pv[j];

			if (ls->maxBBox[j] < ls->pv[j])
				ls->maxBBox[j] = ls->pv[j];
		}

		len = len_v3v3(fv[i], fv[(i + 1) % 3]);
		if (_minEdgeSize > len)
			_minEdgeSize = len;

		*ls->pvi = ls->currentIndex;
		*ls->pni = ls->currentIndex;
		*ls->pmi = ls->currentMIndex;

		ls->currentIndex += 3;
		ls->pv += 3;
		ls->pn += 3;

		ls->pvi++;
		ls->pni++;
		ls->pmi++;
	}

	if (fm)
		marks |= IndexedFaceSet::FACE_MARK;
	if (em1)
		marks |= IndexedFaceSet::EDGE_MARK_V1V2;
	if (em2)
		marks |= IndexedFaceSet::EDGE_MARK_V2V3;
	if (em3)
		marks |= IndexedFaceSet::EDGE_MARK_V3V1;
	*(ls->pm++) = marks;
}

// With A, B and P indicating the three vertices of a given triangle, returns:
// 1 if points A and B are in the same position in the 3D space;
// 2 if the distance between point P and line segment AB is zero; and
// zero otherwise.
int BlenderFileLoader::testDegenerateTriangle(float v1[3], float v2[3], float v3[3])
{
	const float eps = 1.0e-6;
	const float eps_sq = eps * eps;

#if 0
	float area = area_tri_v3(v1, v2, v3);
	bool verbose = (area < 1.0e-6);
#endif

	if (equals_v3v3(v1, v2) || equals_v3v3(v2, v3) || equals_v3v3(v1, v3)) {
#if 0
		if (verbose && G.debug & G_DEBUG_FREESTYLE) {
			printf("BlenderFileLoader::testDegenerateTriangle = 1\n");
		}
#endif
		return 1;
	}
	if (dist_squared_to_line_segment_v3(v1, v2, v3) < eps_sq ||
	    dist_squared_to_line_segment_v3(v2, v1, v3) < eps_sq ||
	    dist_squared_to_line_segment_v3(v3, v1, v2) < eps_sq)
	{
#if 0
		if (verbose && G.debug & G_DEBUG_FREESTYLE) {
			printf("BlenderFileLoader::testDegenerateTriangle = 2\n");
		}
#endif
		return 2;
	}
#if 0
	if (verbose && G.debug & G_DEBUG_FREESTYLE) {
		printf("BlenderFileLoader::testDegenerateTriangle = 0\n");
	}
#endif
	return 0;
}

// Checks if edge rotation (if necessary) can prevent the given quad from
// being decomposed into a degenerate triangle
bool BlenderFileLoader::testEdgeRotation(float v1[3], float v2[3], float v3[3], float v4[3])
{
	if (testDegenerateTriangle(v1, v2, v3) == 2 || testDegenerateTriangle(v1, v3, v4) == 2) {
		if (testDegenerateTriangle(v1, v2, v4) == 2 || testDegenerateTriangle(v2, v3, v4) == 2) {
#if 0
			if (G.debug & G_DEBUG_FREESTYLE) {
				printf("BlenderFileLoader::testEdgeRotation: edge rotation is unsuccessful.\n");
			}
#endif
			return false;
		}
		return true;
	}
	return false;
}

void BlenderFileLoader::insertShapeNode(ObjectInstanceRen *obi, int id)
{
	ObjectRen *obr = obi->obr;
	char *name = obi->ob->id.name + 2;

	// We parse vlak nodes and count the number of faces after the clipping by
	// the near and far view planes is applied (Note: mesh vertices are in the
	// camera coordinate system).
	VlakRen *vlr = NULL;
	unsigned numFaces = 0;
	float v1[3], v2[3], v3[3], v4[3];
	float n1[3], n2[3], n3[3], n4[3], facenormal[3];
	int clip_1[3], clip_2[3];
	int wire_material = 0;
	for (int a = 0; a < obr->totvlak; a++) {
		if ((a & 255) == 0)
			vlr = obr->vlaknodes[a>>8].vlak;
		else
			vlr++;
		if (vlr->mat->mode & MA_ONLYCAST)
			continue;
		if (vlr->mat->material_type == MA_TYPE_WIRE) {
			wire_material = 1;
			continue;
		}
		copy_v3_v3(v1, vlr->v1->co);
		copy_v3_v3(v2, vlr->v2->co);
		copy_v3_v3(v3, vlr->v3->co);
		if (vlr->v4)
			copy_v3_v3(v4, vlr->v4->co);
		if (obi->flag & R_TRANSFORMED) {
			mul_m4_v3(obi->mat, v1);
			mul_m4_v3(obi->mat, v2);
			mul_m4_v3(obi->mat, v3);
			if (vlr->v4)
				mul_m4_v3(obi->mat, v4);
		}
		v1[2] += _z_offset;
		v2[2] += _z_offset;
		v3[2] += _z_offset;
		if (vlr->v4)
			v4[2] += _z_offset;
#if 0
		print_v3("v1", v1);
		print_v3("v2", v2);
		print_v3("v3", v3);
		if (vlr->v4)
			print_v3("v4", v4);
#endif
		if (!vlr->v4 || !testEdgeRotation(v1, v2, v3, v4)) {
			numFaces += countClippedFaces(v1, v2, v3, clip_1);
			if (vlr->v4)
				numFaces += countClippedFaces(v1, v3, v4, clip_2);
		}
		else {
			numFaces += countClippedFaces(v1, v2, v4, clip_1);
			numFaces += countClippedFaces(v2, v3, v4, clip_2);
		}
	}
	if (wire_material) {
		if (G.debug & G_DEBUG_FREESTYLE) {
			cout << "Warning: Object " << name << " has wire materials (ignored)" << endl;
		}
	}
#if 0
	if (G.debug & G_DEBUG_FREESTYLE) {
		cout << "numFaces " << numFaces << endl;
	}
#endif
	if (numFaces == 0)
		return;

	// We allocate memory for the meshes to be imported
	NodeGroup *currentMesh = new NodeGroup;
	NodeShape *shape = new NodeShape;

	unsigned vSize = 3 * 3 * numFaces;
	float *vertices = new float[vSize];
	unsigned nSize = vSize;
	float *normals = new float[nSize];
	unsigned *numVertexPerFaces = new unsigned[numFaces];
	vector<Material *> meshMaterials;
	vector<FrsMaterial> meshFrsMaterials;

	IndexedFaceSet::TRIANGLES_STYLE *faceStyle = new IndexedFaceSet::TRIANGLES_STYLE[numFaces];
	unsigned i;
	for (i = 0; i <numFaces; i++) {
		faceStyle[i] = IndexedFaceSet::TRIANGLES;
		numVertexPerFaces[i] = 3;
	}

	IndexedFaceSet::FaceEdgeMark *faceEdgeMarks = new IndexedFaceSet::FaceEdgeMark[numFaces];

	unsigned viSize = 3 * numFaces;
	unsigned *VIndices = new unsigned[viSize];
	unsigned niSize = viSize;
	unsigned *NIndices = new unsigned[niSize];
	unsigned *MIndices = new unsigned[viSize]; // Material Indices

	struct LoaderState ls;
	ls.pv = vertices;
	ls.pn = normals;
	ls.pm = faceEdgeMarks;
	ls.pvi = VIndices;
	ls.pni = NIndices;
	ls.pmi = MIndices;
	ls.currentIndex = 0;
	ls.currentMIndex = 0;

	FrsMaterial tmpMat;

	// We parse the vlak nodes again and import meshes while applying the clipping
	// by the near and far view planes.
	int p;
	for (p = 0; p < obr->totvlak; ++p) { // we parse the faces of the mesh
		if ((p & 255) == 0)
			vlr = obr->vlaknodes[p>>8].vlak;
		else
			vlr++;
		if ((vlr->mat->mode & MA_ONLYCAST) || vlr->mat->material_type == MA_TYPE_WIRE)
			continue;
		copy_v3_v3(v1, vlr->v1->co);
		copy_v3_v3(v2, vlr->v2->co);
		copy_v3_v3(v3, vlr->v3->co);
		if (vlr->v4)
			copy_v3_v3(v4, vlr->v4->co);
		if (obi->flag & R_TRANSFORMED) {
			mul_m4_v3(obi->mat, v1);
			mul_m4_v3(obi->mat, v2);
			mul_m4_v3(obi->mat, v3);
			if (vlr->v4)
				mul_m4_v3(obi->mat, v4);
		}
		v1[2] += _z_offset;
		v2[2] += _z_offset;
		v3[2] += _z_offset;
		if (vlr->v4)
			v4[2] += _z_offset;
		if (_smooth && (vlr->flag & R_SMOOTH)) {
			copy_v3_v3(n1, vlr->v1->n);
			copy_v3_v3(n2, vlr->v2->n);
			copy_v3_v3(n3, vlr->v3->n);
			if (vlr->v4)
				copy_v3_v3(n4, vlr->v4->n);
			if (obi->flag & R_TRANSFORMED) {
				mul_m3_v3(obi->nmat, n1);
				mul_m3_v3(obi->nmat, n2);
				mul_m3_v3(obi->nmat, n3);
				normalize_v3(n1);
				normalize_v3(n2);
				normalize_v3(n3);
				if (vlr->v4) {
					mul_m3_v3(obi->nmat, n4);
					normalize_v3(n4);
				}
			}
		}
		else {
			RE_vlakren_get_normal(_re, obi, vlr, facenormal);
#ifndef NDEBUG
			/* test if normals are inverted in rendering [T39669] */
			float tnor[3];
			if (vlr->v4)
				normal_quad_v3(tnor, v4, v3, v2, v1);
			else
				normal_tri_v3(tnor, v3, v2, v1);
			BLI_assert(dot_v3v3(tnor, facenormal) > 0.0f);
#endif
			copy_v3_v3(n1, facenormal);
			copy_v3_v3(n2, facenormal);
			copy_v3_v3(n3, facenormal);
			if (vlr->v4)
				copy_v3_v3(n4, facenormal);
		}

		unsigned int numTris_1, numTris_2;
		bool edge_rotation;
		if (!vlr->v4 || !testEdgeRotation(v1, v2, v3, v4)) {
			numTris_1 = countClippedFaces(v1, v2, v3, clip_1);
			numTris_2 = (!vlr->v4) ? 0 : countClippedFaces(v1, v3, v4, clip_2);
			edge_rotation = false;
		}
		else {
			numTris_1 = countClippedFaces(v1, v2, v4, clip_1);
			numTris_2 = countClippedFaces(v2, v3, v4, clip_2);
			edge_rotation = true;
			if (G.debug & G_DEBUG_FREESTYLE) {
				printf("BlenderFileLoader::insertShapeNode: edge rotation is performed.\n");
			}
		}
		if (numTris_1 == 0 && numTris_2 == 0)
			continue;
		bool fm, em1, em2, em3, em4;
		fm = (vlr->freestyle_face_mark) != 0;
		em1 = (vlr->freestyle_edge_mark & R_EDGE_V1V2) != 0;
		em2 = (vlr->freestyle_edge_mark & R_EDGE_V2V3) != 0;
		if (!vlr->v4) {
			em3 = (vlr->freestyle_edge_mark & R_EDGE_V3V1) != 0;
			em4 = false;
		}
		else {
			em3 = (vlr->freestyle_edge_mark & R_EDGE_V3V4) != 0;
			em4 = (vlr->freestyle_edge_mark & R_EDGE_V4V1) != 0;
		}

		Material *mat = vlr->mat;
		if (mat) {
			tmpMat.setLine(mat->line_col[0], mat->line_col[1], mat->line_col[2], mat->line_col[3]);
			tmpMat.setDiffuse(mat->r, mat->g, mat->b, mat->alpha);
			tmpMat.setSpecular(mat->specr, mat->specg, mat->specb, mat->spectra);
			float s = 1.0 * (mat->har + 1) / 4 ; // in Blender: [1;511] => in OpenGL: [0;128]
			if (s > 128.f)
				s = 128.f;
			tmpMat.setShininess(s);
			tmpMat.setPriority(mat->line_priority);
		}

		if (meshMaterials.empty()) {
			meshMaterials.push_back(mat);
			meshFrsMaterials.push_back(tmpMat);
			shape->setFrsMaterial(tmpMat);
		}
		else {
			// find if the Blender material is already in the list
			unsigned int i = 0;
			bool found = false;

			for (vector<Material *>::iterator it = meshMaterials.begin(), itend = meshMaterials.end();
			     it != itend;
			     it++, i++)
			{
				if (*it == mat) {
					ls.currentMIndex = i;
					found = true;
					break;
				}
			}

			if (!found) {
				meshMaterials.push_back(mat);
				meshFrsMaterials.push_back(tmpMat);
				ls.currentMIndex = meshFrsMaterials.size() - 1;
			}
		}

		float triCoords[5][3], triNormals[5][3];
		bool edgeMarks[5]; // edgeMarks[i] is for the edge between i-th and (i+1)-th vertices

		if (numTris_1 > 0) {
			if (!edge_rotation) {
				clipTriangle(numTris_1, triCoords, v1, v2, v3, triNormals, n1, n2, n3,
				             edgeMarks, em1, em2, (!vlr->v4) ? em3 : false, clip_1);
			}
			else {
				clipTriangle(numTris_1, triCoords, v1, v2, v4, triNormals, n1, n2, n4,
				             edgeMarks, em1, false, em4, clip_1);
			}
			for (i = 0; i < numTris_1; i++) {
				addTriangle(&ls, triCoords[0], triCoords[i + 1], triCoords[i + 2],
				            triNormals[0], triNormals[i + 1], triNormals[i + 2],
				            fm, (i == 0) ? edgeMarks[0] : false, edgeMarks[i + 1],
				            (i == numTris_1 - 1) ? edgeMarks[i + 2] : false);
				_numFacesRead++;
			}
		}

		if (numTris_2 > 0) {
			if (!edge_rotation) {
				clipTriangle(numTris_2, triCoords, v1, v3, v4, triNormals, n1, n3, n4,
				             edgeMarks, false, em3, em4, clip_2);
			}
			else {
				clipTriangle(numTris_2, triCoords, v2, v3, v4, triNormals, n2, n3, n4,
				             edgeMarks, em2, em3, false, clip_2);
			}
			for (i = 0; i < numTris_2; i++) {
				addTriangle(&ls, triCoords[0], triCoords[i + 1], triCoords[i + 2],
				            triNormals[0], triNormals[i + 1], triNormals[i + 2],
				            fm, (i == 0) ? edgeMarks[0] : false, edgeMarks[i + 1],
				            (i == numTris_2 - 1) ? edgeMarks[i + 2] : false);
				_numFacesRead++;
			}
		}
	}

	// We might have several times the same vertex. We want a clean 
	// shape with no real-vertex. Here, we are making a cleaning pass.
	real *cleanVertices = NULL;
	unsigned int cvSize;
	unsigned int *cleanVIndices = NULL;

	GeomCleaner::CleanIndexedVertexArray(vertices, vSize, VIndices, viSize, &cleanVertices, &cvSize, &cleanVIndices);

	real *cleanNormals = NULL;
	unsigned int cnSize;
	unsigned int *cleanNIndices = NULL;

	GeomCleaner::CleanIndexedVertexArray(normals, nSize, NIndices, niSize, &cleanNormals, &cnSize, &cleanNIndices);

	// format materials array
	FrsMaterial **marray = new FrsMaterial *[meshFrsMaterials.size()];
	unsigned int mindex = 0;
	for (vector<FrsMaterial>::iterator m = meshFrsMaterials.begin(), mend = meshFrsMaterials.end(); m != mend; ++m) {
		marray[mindex] = new FrsMaterial(*m);
		++mindex;
	}

	// deallocates memory:
	delete [] vertices;
	delete [] normals;
	delete [] VIndices;
	delete [] NIndices;

	// Fix for degenerated triangles
	// A degenerate triangle is a triangle such that
	// 1) A and B are in the same position in the 3D space; or
	// 2) the distance between point P and line segment AB is zero.
	// Only those degenerate triangles in the second form are resolved here
	// by adding a small offset to P, whereas those in the first form are
	// addressed later in WShape::MakeFace().
	vector<detri_t> detriList;
	Vec3r zero(0.0, 0.0, 0.0);
	unsigned vi0, vi1, vi2;
	for (i = 0; i < viSize; i += 3) {
		detri_t detri;
		vi0 = cleanVIndices[i];
		vi1 = cleanVIndices[i + 1];
		vi2 = cleanVIndices[i + 2];
		Vec3r v0(cleanVertices[vi0], cleanVertices[vi0 + 1], cleanVertices[vi0 + 2]);
		Vec3r v1(cleanVertices[vi1], cleanVertices[vi1 + 1], cleanVertices[vi1 + 2]);
		Vec3r v2(cleanVertices[vi2], cleanVertices[vi2 + 1], cleanVertices[vi2 + 2]);
		if (v0 == v1 || v0 == v2 || v1 == v2) {
			continue; // do nothing for now
		}
		else if (GeomUtils::distPointSegment<Vec3r>(v0, v1, v2) < 1.0e-6) {
			detri.viP = vi0;
			detri.viA = vi1;
			detri.viB = vi2;
		}
		else if (GeomUtils::distPointSegment<Vec3r>(v1, v0, v2) < 1.0e-6) {
			detri.viP = vi1;
			detri.viA = vi0;
			detri.viB = vi2;
		}
		else if (GeomUtils::distPointSegment<Vec3r>(v2, v0, v1) < 1.0e-6) {
			detri.viP = vi2;
			detri.viA = vi0;
			detri.viB = vi1;
		}
		else {
			continue;
		}

		detri.v = zero;
		detri.n = 0;
		for (unsigned int j = 0; j < viSize; j += 3) {
			if (i == j)
				continue;
			vi0 = cleanVIndices[j];
			vi1 = cleanVIndices[j + 1];
			vi2 = cleanVIndices[j + 2];
			Vec3r v0(cleanVertices[vi0], cleanVertices[vi0 + 1], cleanVertices[vi0 + 2]);
			Vec3r v1(cleanVertices[vi1], cleanVertices[vi1 + 1], cleanVertices[vi1 + 2]);
			Vec3r v2(cleanVertices[vi2], cleanVertices[vi2 + 1], cleanVertices[vi2 + 2]);
			if (detri.viP == vi0 && (detri.viA == vi1 || detri.viB == vi1)) {
				detri.v += (v2 - v0);
				detri.n++;
			}
			else if (detri.viP == vi0 && (detri.viA == vi2 || detri.viB == vi2)) {
				detri.v += (v1 - v0);
				detri.n++;
			}
			else if (detri.viP == vi1 && (detri.viA == vi0 || detri.viB == vi0)) {
				detri.v += (v2 - v1);
				detri.n++;
			}
			else if (detri.viP == vi1 && (detri.viA == vi2 || detri.viB == vi2)) {
				detri.v += (v0 - v1);
				detri.n++;
			}
			else if (detri.viP == vi2 && (detri.viA == vi0 || detri.viB == vi0)) {
				detri.v += (v1 - v2);
				detri.n++;
			}
			else if (detri.viP == vi2 && (detri.viA == vi1 || detri.viB == vi1)) {
				detri.v += (v0 - v2);
				detri.n++;
			}
		}
		if (detri.n > 0) {
			detri.v.normalizeSafe();
		}
		detriList.push_back(detri);
	}

	if (detriList.size() > 0) {
		vector<detri_t>::iterator v;
		for (v = detriList.begin(); v != detriList.end(); v++) {
			detri_t detri = (*v);
			if (detri.n == 0) {
				cleanVertices[detri.viP]     = cleanVertices[detri.viA];
				cleanVertices[detri.viP + 1] = cleanVertices[detri.viA + 1];
				cleanVertices[detri.viP + 2] = cleanVertices[detri.viA + 2];
			}
			else if (detri.v.norm() > 0.0) {
				cleanVertices[detri.viP]     += 1.0e-5 * detri.v.x();
				cleanVertices[detri.viP + 1] += 1.0e-5 * detri.v.y();
				cleanVertices[detri.viP + 2] += 1.0e-5 * detri.v.z();
			}
		}
		if (G.debug & G_DEBUG_FREESTYLE) {
			printf("Warning: Object %s contains %lu degenerated triangle%s (strokes may be incorrect)\n",
			       name, (long unsigned int)detriList.size(), (detriList.size() > 1) ? "s" : "");
		}
	}

	// Create the IndexedFaceSet with the retrieved attributes
	IndexedFaceSet *rep;
	rep = new IndexedFaceSet(cleanVertices, cvSize, cleanNormals, cnSize, marray, meshFrsMaterials.size(), 0, 0,
	                         numFaces, numVertexPerFaces, faceStyle, faceEdgeMarks, cleanVIndices, viSize,
	                         cleanNIndices, niSize, MIndices, viSize, 0, 0, 0);
	// sets the id of the rep
	rep->setId(Id(id, 0));
	rep->setName(obi->ob->id.name + 2);

	const BBox<Vec3r> bbox = BBox<Vec3r>(Vec3r(ls.minBBox[0], ls.minBBox[1], ls.minBBox[2]),
	                                     Vec3r(ls.maxBBox[0], ls.maxBBox[1], ls.maxBBox[2]));
	rep->setBBox(bbox);
	shape->AddRep(rep);

	currentMesh->AddChild(shape);
	_Scene->AddChild(currentMesh);
}

} /* namespace Freestyle */
