#include <u.h>
#include <libc.h>
#include <thread.h>
#include <draw.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>

#define STACKSIZE       (8 * 1024)
#define TIME            30
#define TILESX          32
#define TILESY          20
#define FACT            1024
#define WIDTH           (TILESX * FACT)
#define HEIGHT          (TILESY * FACT)
#define MINCAPTURED     0.60
#define MAXBALLS        50

/* default colors */
#define COLOR_BALL      DMedgreen
#define COLOR_BOARD     DPurpleblue
#define COLOR_WALL      DBlack
#define COLOR_EXT0      DBlue
#define COLOR_EXT1      DRed
#define COLOR_BARBG     DWhite
#define COLOR_BARFG     DBlack

/* orientation of the cursor */
enum Orientation {
	HORZ,
	VERT,
};

/* building status of a wall extension */
enum Extension {
	EXT_INACTIVE = -1,
	EXT_BUILDING = 0,
	EXT_BUILT    = 1,
};

/* a ball is given by a point and the direction */
struct Ball {
	Point p;                /* position of top-left corner of square around ball */
	int dx, dy;             /* ball direction */
};

/* the wall being constructed */
struct Wall {
	Point p;                /* position of wall */
	int s0, s1;             /* size of wall; s0 goes negative, while s1 goes positive */
	enum Extension e0, e1;  /* status of each wall extension */
	enum Orientation o;     /* orientation of wall */
};

/* button-1 click info given from the mouse thread to the game thread */
struct Click {
	Point p;                /* position of click */
	enum Orientation o;     /* orientation of cursor */
};

/* global variables */
Channel *drawc;                 /* used by the resize and clock threads to inform the game thread to redraw stuff */
Channel *clickc;                /* used by the mouse thread to inform the game thread that a button-1 click was performed */
Image *ballimg, *boardimg, *wallimg, *ext0img, *ext1img, *barbgimg, *barfgimg;
Mousectl *mctl;
Keyboardctl *kctl;

/* horizontal double-arrow cursor */
Cursor horz = {
	{-8, -8},
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	  0x00, 0x00, 0x10, 0x08, 0x30, 0x0c, 0x7f, 0xfe,
	  0x7f, 0xfe, 0x30, 0x0c, 0x10, 0x08, 0x00, 0x00,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	},
	{ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	  0x18, 0x18, 0x28, 0x14, 0x4f, 0xf2, 0x80, 0x01,
	  0x80, 0x01, 0x4f, 0xf2, 0x28, 0x14, 0x18, 0x18,
	  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	},
};

/* vertical double-arrow cursor */
Cursor vert = {
	{-8, -8},
	{ 0x00, 0x00, 0x01, 0x80, 0x03, 0xc0, 0x07, 0xe0,
	  0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80,
	  0x01, 0x80, 0x01, 0x80, 0x01, 0x80, 0x01, 0x80,
	  0x07, 0xe0, 0x03, 0xc0, 0x01, 0x80, 0x00, 0x00,
	},
	{ 0x01, 0x80, 0x02, 0x40, 0x04, 0x20, 0x08, 0x10,
	  0x0e, 0x70, 0x02, 0x40, 0x02, 0x40, 0x02, 0x40,
	  0x02, 0x40, 0x02, 0x40, 0x02, 0x40, 0x0e, 0x70,
	  0x08, 0x10, 0x04, 0x20, 0x02, 0x40, 0x01, 0x80,
	},
};

/* call allocimage(2) checking for error */
static Image *
eallocimage(Rectangle r, ulong chan, int repl, ulong col)
{
	Image *img;

	if((img = allocimage(display, r, chan, repl, col)) == nil)
		sysfatal("allocimage: %r");
	return img;
}

/* initialize color images */
static void
initimgs(void)
{
	ballimg = eallocimage(Rect(0,0,1,1), screen->chan, 1, COLOR_BALL);
	boardimg = eallocimage(Rect(0,0,1,1), screen->chan, 1, COLOR_BOARD);
	wallimg = eallocimage(Rect(0,0,1,1), screen->chan, 1, COLOR_WALL);
	ext0img = eallocimage(Rect(0,0,1,1), screen->chan, 1, COLOR_EXT0);
	ext1img = eallocimage(Rect(0,0,1,1), screen->chan, 1, COLOR_EXT1);
	barbgimg = eallocimage(Rect(0,0,1,1), screen->chan, 1, COLOR_BARBG);
	barfgimg = eallocimage(Rect(0,0,1,1), screen->chan, 1, COLOR_BARFG);
}

/* close channels, keyboard, mouse, and display; and exit the threads */
static void
terminate(void)
{
	chanclose(drawc);
	chanclose(clickc);
	closekeyboard(kctl);
	closemouse(mctl);
	closedisplay(display);
	threadexitsall(nil);
}

/* set as 1 (captured) the borders of the board */
static void
tilesinit(int tiles[TILESX][TILESY])
{
	int i;

	for(i = 0; i < TILESX; i++)
		tiles[i][0] = tiles[i][TILESY - 1] = 1;
	for(i = 0; i < TILESY; i++)
		tiles[0][i] = tiles[TILESX - 1][i] = 1;
}

/* set as 0 (not captured) the interior of the board */
static void
tilesclear(int tiles[TILESX][TILESY])
{
	int i, j;

	for(i = 1; i < TILESX - 1; i++){
		for(j = 1; j < TILESY - 1; j++){
			tiles[i][j] = 0;
		}
	}
}

/* compute size of the board; called after resizing the window */
static void
boardcalc(Point *p, int *s)
{
	double d;
	int w, h;

	w = Dx(screen->r);
	h = Dy(screen->r) - font->height;
	if((double)w/h < (double)WIDTH/HEIGHT) {
		d = (double)w * (double)HEIGHT / (double)WIDTH;
		*s = w / TILESX;
		p->x = screen->r.min.x + (w - *s * TILESX) / 2;
		p->y = screen->r.min.y + (h - d) / 2;
	}else{
		d = (double)h * (double)WIDTH / (double)HEIGHT;
		*s = h / TILESY;
		p->x = screen->r.min.x + (w - d) / 2;
		p->y = screen->r.min.y + (h - *s * TILESY) / 2;
	}
}

/* check whether there is a ball in the given x, y position */
static int
hasball(struct Ball *balls, int nballs, int x, int y)
{
	int i;

	for(i = 0; i < nballs; i++)
		if(balls[i].p.x == x && balls[i].p.y == y)
			return 1;
	return 0;
}

/* draw board and everything in it */
static void
boarddraw(int tiles[TILESX][TILESY], struct Ball *balls, struct Wall *wall, Point *orig, int fact, int nballs)
{
	int i, j;
	Rectangle r;
	Point p;

	draw(screen, screen->r, boardimg, nil, ZP);

	/* draw wall extensions */
	if(wall->e0 == EXT_BUILDING){
		if(wall->o == HORZ){
			r.max.x = orig->x + wall->p.x * fact;
			r.max.y = orig->y + wall->p.y * fact + fact;
			r.min.x = r.max.x + (wall->s0 * fact);
			r.min.y = orig->y + wall->p.y * fact;
			draw(screen, r, ext0img, nil, ZP);
		}else{
			r.max.y = orig->y + wall->p.y * fact;
			r.max.x = orig->x + wall->p.x * fact + fact;
			r.min.y = r.max.y + (wall->s0 * fact);
			r.min.x = orig->x + wall->p.x * fact;
			draw(screen, r, ext0img, nil, ZP);
		}
	}
	if(wall->e1 == EXT_BUILDING){
		if(wall->o == HORZ){
			r.min.x = orig->x + wall->p.x * fact;
			r.min.y = orig->y + wall->p.y * fact;
			r.max.x = r.min.x + (wall->s1 * fact);
			r.max.y = orig->y + wall->p.y * fact + fact;
			draw(screen, r, ext1img, nil, ZP);
		}else{
			r.min.y = orig->y + wall->p.y * fact;
			r.min.x = orig->x + wall->p.x * fact;
			r.max.y = r.min.y + (wall->s1 * fact);
			r.max.x = orig->x + wall->p.x * fact + fact;
			draw(screen, r, ext1img, nil, ZP);
		}
	}

	/* draw captured tiles */
	for(i = 0; i < TILESX; i++){
		for(j = 0; j < TILESY; j++){
			if(tiles[i][j]){
				r.min.x = orig->x + i * fact;
				r.min.y = orig->y + j * fact;
				r.max.x = r.min.x + fact;
				r.max.y = r.min.y + fact;
				draw(screen, r, wallimg, nil, ZP);
			}
		}
	}

	/* draw balls */
	for(i = 0; i < nballs; i++){
		p.x = orig->x + balls[i].p.x * fact + fact / 2;
		p.y = orig->y + balls[i].p.y * fact + fact / 2;
		fillellipse(screen, p, fact / 2, fact / 2, ballimg, ZP);
	}
}

/* draw the status bar */
static void
statusdraw(int lvl, int nlives, double captured)
{
	Rectangle r;
	Point p;
	int w;
	char buf[128];

	r = screen->r;
	r.min.y = r.max.y - font->height;
	draw(screen, r, barbgimg, nil, ZP);

	snprint(buf, sizeof(buf), "lvl: %d; nlives: %d; captured: %.1f%%", lvl, nlives, captured * 100.0);
	w = stringwidth(font, buf);
	p.x = screen->r.min.x + (Dx(screen->r) - w) / 2;
	p.y = screen->r.max.y - font->height;
	string(screen, p, barfgimg, ZP, font, buf);
}

/* place balls in the board; called when changing the level */
static int
newlvl(struct Ball *balls, int lvl)
{
	int nballs;
	int i;

	nballs = lvl + 1;
	if(nballs > MAXBALLS)
		nballs = MAXBALLS;
	for(i = 0; i < nballs; i++){
		balls[i].p.x = 4 + ntruerand(TILESX - 8);
		balls[i].p.y = 4 + ntruerand(TILESY - 8);
		balls[i].dx = ntruerand(2) % 2 ? 1 : -1;
		balls[i].dy = ntruerand(2) % 2 ? 1 : -1;
	}
	return nballs;
}

/* get position in the board (in TILESX * TILESY) given a position in the window */
static Point
gettile(Point click, Point orig, int fact)
{
	Point p;

	p = Pt(0,0);
	if(click.x >= orig.x && click.x < orig.x + TILESX * fact &&
	   click.y >= orig.y && click.y < orig.y + TILESY * fact){
		p.x = (click.x - orig.x) / fact;
		p.y = (click.y - orig.y) / fact;
	}
	return p;
}

/* get ratio of captured inner tiles (not counting the boarders) per number of tiles */
static double
getcaptured(int tiles[TILESX][TILESY])
{
	int i, j, n;

	n = 0;
	for(i = 1; i < TILESX - 1; i++)
		for(j = 1; j < TILESY - 1; j++)
			if(tiles[i][j])
				n++;
	return (double)n/(TILESX * TILESY);
}

/* build walls, bounce balls, do collision detection; return life difference */
static int
gameupdate(int tiles[TILESX][TILESY], struct Ball *balls, struct Wall *wall, int nballs)
{
	int i, j, x, y;
	int life;
	int cons0, cons1;
	int bx, by;

	life = 0;

	/* extend extension and check if ball crosses them */
	if(wall->e0 == EXT_BUILDING){
		wall->s0--;
		if(wall->o == VERT){
			for(i = wall->p.y + wall->s0; i <= wall->p.y; i++){
				if(hasball(balls, nballs, wall->p.x, i)){
					life = -1;
					wall->e0 = EXT_INACTIVE;
				}
			}
		}else{
			for(i = wall->p.x + wall->s0; i <= wall->p.x; i++){
				if(hasball(balls, nballs, i, wall->p.y)){
					life = -1;
					wall->e0 = EXT_INACTIVE;
				}
			}
		}
	}
	if(wall->e1 == EXT_BUILDING){
		wall->s1++;
		if(wall->o == VERT){
			for(i = wall->p.y; i <= wall->p.y + wall->s1; i++){
				if(hasball(balls, nballs, wall->p.x, i)){
					life = -1;
					wall->e1 = EXT_INACTIVE;
				}
			}
		}else{
			for(i = wall->p.x; i <= wall->p.x + wall->s1; i++){
				if(hasball(balls, nballs, i, wall->p.y)){
					life = -1;
					wall->e1 = EXT_INACTIVE;
				}
			}
		}
	}

	/* check if each extension was built */
	if(wall->e0 == EXT_BUILDING){
		if(wall->o == VERT){
			i = wall->p.y + wall->s0;
			if(tiles[wall->p.x][i]){
				for(j = i; j <= wall->p.y; j++)
					tiles[wall->p.x][j] = 1;
				wall->e0 = EXT_BUILT;
			}
		}else{
			i = wall->p.x + wall->s0;
			if(tiles[i][wall->p.y]){
				for(j = i; j <= wall->p.x; j++)
					tiles[j][wall->p.y] = 1;
				wall->e0 = EXT_BUILT;
			}
		}
	}
	if(wall->e1 == EXT_BUILDING){
		if(wall->o == VERT){
			i = wall->p.y + wall->s1;
			if(tiles[wall->p.x][i]){
				for(j = wall->p.y; j <= i; j++)
					tiles[wall->p.x][j] = 1;
				wall->e1 = EXT_BUILT;
			}
		}else{
			i = wall->p.x + wall->s1;
			if(tiles[i][wall->p.y]){
				for(j = wall->p.x; j <= i; j++)
					tiles[j][wall->p.y] = 1;
				wall->e1 = EXT_BUILT;
			}
		}
	}

	/* capture areas */
	cons0 = cons1 = 1;
	if((wall->e0 == EXT_BUILT && wall->e1 == EXT_INACTIVE) ||
	   (wall->e1 == EXT_BUILT && wall->e0 == EXT_INACTIVE)){
		wall->e0 = wall->e1 = EXT_INACTIVE;
	}else if(wall->e0 == EXT_BUILT && wall->e1 == EXT_BUILT){
		if(wall->o == VERT){
			for(i = wall->s0; (cons0 || cons1) && i <= wall->s1; i++){
				for(j = wall->p.x - 1; cons0 && !tiles[j][wall->p.y + i]; j--){
					if(hasball(balls, nballs, j, wall->p.y + i)){
						cons0 = 0;
						break;
					}
				}
				for(j = wall->p.x + 1; cons1 && !tiles[j][wall->p.y + i]; j++){
					if(hasball(balls, nballs, j, wall->p.y + i)){
						cons1 = 0;
						break;
					}
				}
			}
			for(i = wall->s0; (cons0 || cons1) && i <= wall->s1; i++){
				for(j = wall->p.x - 1; cons0 && !tiles[j][wall->p.y + i]; j--){
					tiles[j][wall->p.y + i] = 1;
				}
				for(j = wall->p.x + 1; cons1 && !tiles[j][wall->p.y + i]; j++){
					tiles[j][wall->p.y + i] = 1;
				}
			}
		}else{
			for(i = wall->s0; (cons0 || cons1) && i <= wall->s1; i++){
				for(j = wall->p.y - 1; cons0 && !tiles[wall->p.x + i][j]; j--){
					if(hasball(balls, nballs, wall->p.x + i, j)){
						cons0 = 0;
						break;
					}
				}
				for(j = wall->p.y + 1; cons1 && !tiles[wall->p.x + i][j]; j++){
					if(hasball(balls, nballs, wall->p.x + i, j)){
						cons1 = 0;
						break;
					}
				}
			}
			for(i = wall->s0; (cons0 || cons1) && i <= wall->s1; i++){
				for(j = wall->p.y - 1; cons0 && !tiles[wall->p.x + i][j]; j--){
					tiles[wall->p.x + i][j] = 1;
				}
				for(j = wall->p.y + 1; cons1 && !tiles[wall->p.x + i][j]; j++){
					tiles[wall->p.x + i][j] = 1;
				}
			}
		}
		wall->e0 = wall->e1 = EXT_INACTIVE;
	}

	for(i = 0; i < nballs; i++){
		/* collision detection */
		x = balls[i].p.x + balls[i].dx;
		y = balls[i].p.y + balls[i].dy;
		if(tiles[x][y]){
			bx = by = 1;
			if((balls[i].dy > 0 && !tiles[x][y - 1]) ||
			   (balls[i].dy < 0 && !tiles[x][y + 1]))
				bx = 0;
			if((balls[i].dx > 0 && !tiles[x - 1][y]) ||
			   (balls[i].dx < 0 && !tiles[x + 1][y]))
				by = 0;
			if(!bx && !by)
				bx = by = 1;
			if(bx)
				balls[i].dx *= -1;
			if(by)
				balls[i].dy *= -1;
		}

		/* update ball position */
		balls[i].p.x += balls[i].dx;
		balls[i].p.y += balls[i].dy;
	}

	return life;
}

/* the game thread */
static void
gamethread(void *)
{
	struct Ball balls[MAXBALLS];
	struct Click click;
	struct Wall wall;
	Point orig;
	double captured;
	int fact;
	int tiles[TILESX][TILESY];
	int lvl, nballs, nlives;
	int haswon;
	int resized;
	Alt alts[] = {
		{drawc, &resized, CHANRCV},
		{clickc, &click, CHANRCV},
		{nil, nil, CHANEND},
	};

	lvl = 1;
	nlives = nballs = newlvl(balls, lvl);
	haswon = 0;
	captured = 0.0;
	for(;;){
		wall.e0 = wall.e1 = EXT_INACTIVE;
		tilesinit(tiles);
		tilesclear(tiles);
		boardcalc(&orig, &fact);
		boarddraw(tiles, balls, &wall, &orig, fact, nballs);
		statusdraw(lvl, nlives, 0.0);
		flushimage(display, 1);
		while(nlives > 0 && !haswon){
			switch(alt(alts)){
			case 0:
				if(resized){
					boardcalc(&orig, &fact);
				}else{
					nlives += gameupdate(tiles, balls, &wall, nballs);
					captured = getcaptured(tiles);
				}
				boarddraw(tiles, balls, &wall, &orig, fact, nballs);
				statusdraw(lvl, nlives, captured);
				flushimage(display, 1);
				haswon = captured >= MINCAPTURED;
				break;
			case 1:
				if(wall.e0 == EXT_INACTIVE && wall.e1 == EXT_INACTIVE){
					wall.p = gettile(click.p, orig, fact);
					if(wall.p.x == 0 || wall.p.y == 0)
						break;
					if(tiles[wall.p.x][wall.p.y])
						break;
					wall.e0 = wall.e1 = EXT_BUILDING;
					wall.s0 = wall.s1 = 0;
					wall.o = click.o;
				}
				break;
			default:
				sysfatal("can't happen");
			}
		}
		if(haswon)
			lvl++;
		else
			lvl = 1;
		nlives = nballs = newlvl(balls, lvl);
		wall.e0 = wall.e1 = EXT_INACTIVE;
		haswon = 0;
	}
}

/* send 0 (no resizing occurred) via drawc to inform the game thread to redraw stuff */
static void
clockproc(void *)
{
	for(;;){
		sleep(TIME);
		sendul(drawc, 0);
	}
}

/* send 1 (resizing occurred) via drawc to inform the game thread to redraw stuff and recompute board size */
static void
resizethread(void *)
{
	for(;;){
		recvul(mctl->resizec);
		if(getwindow(display, Refnone) < 0)
			sysfatal("getwindow: %r");
		sendul(drawc, 1);
	}
}

/* send a click structure to the game thread when a button-1 click occurs */
static void
mousethread(void *)
{
	struct Click click;
	Mouse m;
	int o;

	o = VERT;
	for(;;){
		recv(mctl->c, &m);
		if(m.buttons == (1 << 2)){
			if(o == HORZ){
				setcursor(mctl, &vert);
				o = VERT;
			}else{
				setcursor(mctl, &horz);
				o = HORZ;
			}
		}else if(m.buttons == (1 << 0)){
			click.p = m.xy;
			click.o = o;
			send(clickc, &click);
		}
	}
}

/* just handle Delete and q to quit */
static void
keyboardthread(void *)
{
	Rune r;

	for(;;){
		recv(kctl->c, &r);
		if(r == Kdel || r == 'q'){
			terminate();
		}
	}
}

/* 9ball: build walls to capture 60% of the grid without touching the balls */
void
threadmain(int, char *argv[])
{
	if(initdraw(nil, nil, argv[0]) < 0)
		sysfatal("initdraw: %r");
	if((mctl = initmouse(nil, nil)) == nil)
		sysfatal("initmouse: %r");
	if((kctl = initkeyboard(nil)) == nil)
		sysfatal("initkeyboard: %r");
	srand(time(0));
	initimgs();
	setcursor(mctl, &vert);
	drawc = chancreate(sizeof(ulong), 0);
	clickc = chancreate(sizeof(struct Click), 0);
	proccreate(clockproc, nil, STACKSIZE);
	threadcreate(gamethread, nil, STACKSIZE);
	threadcreate(resizethread, nil, STACKSIZE);
	threadcreate(mousethread, nil, STACKSIZE);
	threadcreate(keyboardthread, nil, STACKSIZE);
	threadexits(nil);
}
