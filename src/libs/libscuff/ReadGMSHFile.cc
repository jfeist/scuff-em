/*
 * ReadGMSHFile.cc -- subroutine of the RWGObject class constructor
 *
 * homer reid    -- 3/2007 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "libscuff.h"

namespace scuff {

/*************************************************************/
/* constants needed in this file only  ***********************/
/*************************************************************/
#define TYPE_TRIANGLE 2
#define TYPE_POINT    15

#define NODE_START_KEYWORD1 "$NOD"
#define NODE_START_KEYWORD2 "$Nodes"

#define NODE_END_KEYWORD1   "$ENDNOD"
#define NODE_END_KEYWORD2   "$EndNodes"

#define ELM_START_KEYWORD1  "$ELM"
#define ELM_START_KEYWORD2  "$Elements"

#define ELM_END_KEYWORD1    "$ENDELM"
#define ELM_END_KEYWORD2    "$EndElements"

#define FORMAT_LEGACY 0
#define FORMAT_NEW    1

#define MAXREFPTS 100

/*************************************************************/
/*************************************************************/
/*************************************************************/
void RWGObject::ReadGMSHFile(FILE *MeshFile, char *FileName, GTransformation *GT)
{
  RWGPanel *P;
  char buffer[100];
  int VI[3], Temp;
  int *GMSH2HR;
  int jGMSH;
  int i, j, nv, ne, np, LineNum, NumElements, NodeIndex;
  int ElNum, ElType, RegPhys, RegElem, NodeCnt, nConv;

  int WhichMeshFormat, KeywordFound, nt, nTags, nRead, iDummy, bufPos;

  /* stuff to figure out correct orientation of panel normals */
  double dRP, dRPMin;
  double *RP, *RPMin;
  double CentroidDisplaced[3];
  int nrp;
  int RefPntIndices[MAXREFPTS];
 
  /*------------------------------------------------------------*/
  /* Read lines until we hit the keyword indicating the start   */
  /* of the 'nodes' section, then read the number of nodes.     */
  /*------------------------------------------------------------*/
  LineNum=0;
  KeywordFound=0;
  while(KeywordFound==0)
   { 
     if (!fgets(buffer,100,MeshFile))
      RWGErrExit("%s: failed to find node start keyword");
     LineNum++;
     if( !strncmp(buffer,NODE_START_KEYWORD1,strlen(NODE_START_KEYWORD1)))
      { WhichMeshFormat=FORMAT_LEGACY; KeywordFound=1; }
     else if( !strncmp(buffer,NODE_START_KEYWORD2,strlen(NODE_START_KEYWORD2)))
      { WhichMeshFormat=FORMAT_NEW; KeywordFound=1; }
   };
  if ( !fgets(buffer,100,MeshFile) || !(NumVertices=atoi(buffer)) )
   RWGErrExit("%s: invalid number of nodes",FileName); 
  LineNum++;

  /*------------------------------------------------------------*/
  /*- Read in the vertice (which GMSH calls 'nodes.')           */
  /*- Note that the numbering of the vertices in GMSH does not  */
  /*- necessarily correspond to their ordering in the mesh      */
  /*- file. To remedy this situation, we construct a mapping    */
  /*- between GMSH's vertices indices and our internal vertex   */
  /*- indices, which works like this: The vertex that GMSH      */
  /*- calls 'node 3' is stored in slot GMSH2HR[3] within our    */
  /*- internal Vertices array.                                  */ 
  /*------------------------------------------------------------*/
  Vertices=(double *)RWGMalloc(3*NumVertices*sizeof(double));
  GMSH2HR=(int *)RWGMalloc(2*NumVertices*sizeof(int));
  for(i=0; i<2*NumVertices; i++)
   GMSH2HR[i]=-1;
  for (nv=0; nv<NumVertices; nv++)
   { if (!fgets(buffer,100,MeshFile))
      RWGErrExit("%s: too few nodes",FileName);
     LineNum++;
     nConv=sscanf(buffer,"%i %le %le %le",&NodeIndex,
                          Vertices+3*nv,Vertices+3*nv+1,Vertices+3*nv+2); 
     if(nConv!=4)
      RWGErrExit("%s:%i: invalid node specification",FileName,LineNum); 
     if (NodeIndex>2*NumVertices)
      RWGErrExit("%s: internal error in ReadGMSHFile",FileName);
     GMSH2HR[NodeIndex]=nv;
   }; /* for (nv=0; nv<NumVertices; nv++) */
   
  /*------------------------------------------------------------*/
  /*- Apply geometrical transformation (if any) to all nodes.  -*/
  /*------------------------------------------------------------*/
  if (GT) ApplyGTransformation(GT, Vertices, NumVertices);
 
  /*------------------------------------------------------------*/
  /*- Eliminate any redundant vertices from the vertex list.   -*/
  /*- Explain me in greater detail please.                     -*/
  /*------------------------------------------------------------*/
  NumRedundantVertices=0;
  for(i=0; i<NumVertices; i++)
   for(j=i+1; j<NumVertices; j++)
    if ( VecDistance(Vertices+3*i, Vertices+3*j) < 1.0e-6 )
     {
       /* remap all references to my node j so that they now refer to my node i*/
       for(jGMSH=0; jGMSH<2*NumVertices; jGMSH++)
        if (GMSH2HR[jGMSH]==j)
         GMSH2HR[jGMSH]=i;

       NumRedundantVertices++;
       //fprintf(stderr,"\n*\n* redundant nodes found!!(%i,%i)\n*\n",i,j);
     };
 
  /*------------------------------------------------------------*/
  /* Confirm that the next two lines in the file are the        */
  /* end-of-nodes-section keyword and the start-of-elements-section */
  /* keyword, then read the number of elements.                 */
  /*------------------------------------------------------------*/
  if ( !fgets(buffer,100,MeshFile) )
   RWGErrExit("%s: bad file format (nodes section not terminated)",FileName);
  LineNum++;

  if ( WhichMeshFormat==FORMAT_LEGACY ) 
   { if ( strncmp(buffer,NODE_END_KEYWORD1,strlen(NODE_END_KEYWORD1)))
      RWGErrExit("%s:%i: unexpected keyword",FileName,LineNum);
   }
  else
   { if ( strncmp(buffer,NODE_END_KEYWORD2,strlen(NODE_END_KEYWORD2)))
      RWGErrExit("%s:%i: unexpected keyword",FileName,LineNum);
   };

  if ( !fgets(buffer,100,MeshFile) )
   RWGErrExit("%s: bad file format (elements section not initiated)",FileName);
  LineNum++;

  if ( WhichMeshFormat==FORMAT_LEGACY ) 
   { if ( strncmp(buffer,ELM_START_KEYWORD1,strlen(ELM_START_KEYWORD1)))
      RWGErrExit("%s:%i: unexpected keyword",FileName,LineNum);
   }
  else
   { if ( strncmp(buffer,ELM_START_KEYWORD2,strlen(ELM_START_KEYWORD2)))
      RWGErrExit("%s:%i: unexpected keyword",FileName,LineNum);
   }

  if ( !fgets(buffer,100,MeshFile) )
   RWGErrExit("%s: bad file format (invalid number of elements)",FileName);
  LineNum++;
  nConv=sscanf(buffer,"%i",&NumElements);
  if (nConv!=1 || NumElements<0) 
   RWGErrExit("%s:%i: invalid number of elements",FileName,LineNum);
 
  /*------------------------------------------------------------*/
  /*- Now read each line of the elements section.               */ 
  /*------------------------------------------------------------*/
  NumPanels=NumRefPts=0;
  Panels=(RWGPanel **)RWGMalloc(NumElements * sizeof(Panels[0]));
  for (ne=0; ne<NumElements; ne++)
   { 
     if (!fgets(buffer,100,MeshFile))
      RWGErrExit("too few elements in input file");
     LineNum++;

     if (WhichMeshFormat==FORMAT_LEGACY)
      { 
        nConv=sscanf(buffer,"%i %i %i %i %i %i %i %i",
                     &ElNum,&ElType,&RegPhys,&RegElem,&NodeCnt,VI,VI+1,VI+2);
        if (nConv<5)
         RWGErrExit("%s:%i: invalid element specification",FileName,LineNum);
      }
     else
      { 
        nConv=sscanf(buffer,"%i %i %i%n",&ElNum,&ElType,&nTags,&nRead);
        if (nConv<3)
         RWGErrExit("%s:%i: invalid element specification",FileName,LineNum);
        bufPos=nRead;
        for(nt=0; nt<nTags; nt++)
         { sscanf(buffer+bufPos,"%i%n",&iDummy,&nRead);
           bufPos+=nRead;
         };
        nConv=sscanf(buffer+bufPos,"%i %i %i",VI,VI+1,VI+2);
        if (ElType==TYPE_TRIANGLE && nConv!=3)
         RWGErrExit("%s:%i: invalid element specification",FileName,LineNum);
        else if (ElType==TYPE_POINT && nConv!=1)
         RWGErrExit("%s:%i: invalid element specification",FileName,LineNum);
      };
   
     /* we only process elements that are points or triangles */
     switch(ElType)
      { 
        /***************************************************************/
        /* add new reference point to list of reference points *********/
        /***************************************************************/
        case TYPE_POINT:
          if (NumRefPts==MAXREFPTS)
           RWGErrExit("%s:%i: too many reference points",FileName,LineNum); 
          RefPntIndices[NumRefPts++]=GMSH2HR[ VI[0] ];
          break;

        /***************************************************************/
        /* add new triangle to list of panels                          */
        /***************************************************************/
        case TYPE_TRIANGLE:
          Panels[NumPanels]=NewRWGPanel(Vertices, GMSH2HR[ VI[0] ], GMSH2HR[ VI[1] ], 
                                        GMSH2HR[ VI[2] ]);
          Panels[NumPanels]->Index=NumPanels;
          NumPanels++;
          break;

        default:
          //RWGErrExit("%s:%i: unknown element type %i",FileName,LineNum,ElType);
          //fprintf(stderr,"%s:%i: warning: ignoring unknown element type %i\n",FileName,LineNum,ElType);
          break;

      }; // switch(ElType)
   }; //for (ne=0; ne<NumElements; ne++)

  /*------------------------------------------------------------*/
  /*- flip panel normals as necessary based on user's          -*/
  /*- specification of reference points.                       -*/
  /*------------------------------------------------------------*/
  if (NumRefPts>0)
   { 
     for(np=0; np<NumPanels; np++)
      { 
        P=Panels[np];

        /* find nearest reference point */
        RPMin=Vertices + 3*RefPntIndices[0];
        dRPMin=VecDistance(P->Centroid, RPMin);
        for(nrp=1; nrp<NumRefPts; nrp++)
         { RP=Vertices+3*RefPntIndices[nrp];
           dRP=VecDistance(P->Centroid, RP);
           if ( dRP < dRPMin)
            { dRPMin=dRP;
              RPMin=RP;
            };
         };

        /* figure out if distance to nearest reference point    */
        /* increases or decreases when we move a small distance */
        /* in the direction of the panel normal                 */
        VecScaleAdd(P->Centroid,1.0e-3,P->ZHat,CentroidDisplaced);
        dRP=VecDistance(CentroidDisplaced,RPMin);

        /* if the distance decreased, then the panel normal is  */
        /* backwards and must be flipped. in this case we must  */
        /* also interchange the labeling of panel vertices 0    */
        /* and 2 to ensure that the right-hand-rule convention  */
        /* is preserved                                         */
        if (dRP < dRPMin) 
         { VecScale(P->ZHat,-1.0);
           Temp=P->VI[0]; P->VI[0]=P->VI[2]; P->VI[2]=Temp;
         };

      }; //for(np=0; np<NumPanels; np++)

   }; // if (NumRefPts>0)

  free(GMSH2HR);
  fclose(MeshFile);
 
} 

} // namespace scuff
